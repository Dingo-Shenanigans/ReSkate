// A cover over the world's upward-facing surfaces (world_cover.cpp, reskate.effects.cover), drawn into
// the finished scene just before the game puts it on the screen with its UI, and snow falling in front
// of it. One full-screen triangle; t0 is a copy of that picture, t1 the game's own depth for it
// (reversed: 1 near, 0 far: our copy of it, which ps_depth takes), and t3 the cameras ps_measure tries
// (a buffer).

cbuffer constants : register(b0) {
    float4 right_focal;   // the camera's right (world), and its focal length in pixels
    float4 up_width;      // its up, and the picture's width
    float4 back_height;   // its back (it looks along -back), and the picture's height
    float4 eye_near;      // where it is, and the near distance the reversed depth is over when not measured
    float4 colour_amount; // the cover's colour, and how much of a covered surface it hides
    float4 shape;         // facing, softness, brightness, breakup
    float4 extra;         // sparkle, how far the snow has fallen (seconds at the usual speed), how much snow falls (0 to 1), how fast (1 the usual)
    float4 clear_info;    // how many places are kept clear (0 to 3), then each one's height above its foot
    float4 clears[3];     // each: its foot in the world, and its radius
    float4 last_right;    // last frame's camera (as this frame's above), and whether it is last frame's (for the flakes' streaks)
    float4 last_up;       // ... (w unused)
    float4 last_back;     // ..., and how much of the cover is left (melting: 1 all of it)
    float4 last_eye;      // ..., and how long the snowfall has been stopping in seconds (-1: falling)
};
Texture2D<float4> scene : register(t0);
Texture2D<float> depth : register(t1);
// The cameras ps_measure tries (world_cover.cpp's camera_candidates): each its right, up, back and eye.
StructuredBuffer<float4> cameras : register(t3);
#define CANDIDATES 1
// After the cameras: three float4s a place kept clear: (how much snow shows in it, -, -, -), then a
// board-shaped one's (forward, half its length) and (up, half its width), w 0 for an upright cylinder.
// Then the trails: [0] = (points, the box they lie in: min x, min z, max x), [1] = (max z, min y, max y,
// -), then two a point, oldest first: (where, width), (how strong it still is, 0 to 1, its tread's width, -).
#define PLACES (CANDIDATES * 4)
#define TRAIL (PLACES + 9)
#define MOST_TRAIL 256
// After the trails, the roof map (world_cover.cpp's measure_roofs): [0] = (on, the near grid's cells across,
// its cell size, the far grid's cells across), [1] = (the far grid's cell size, -, -, -), then the near grid's
// cells, then the far grid's, a float4 a cell kept where its world position falls (x and z each modulo its
// cells across): (the highest surface over the cell's middle, which cell it is: x, z, 1 once measured).
#define ROOF (TRAIL + 2 + MOST_TRAIL * 2)
// Something this far over a surface is a roof over it (a ceiling, a bridge), not a bench or a rail on it.
#define ROOF_CLEARANCE 1.5

struct Vertex { float4 position : SV_Position; };
struct Output {
    float4 colour : SV_Target0;
};

// Our copy of the game's depth (world_cover.cpp, snapshot_depth): t1 is the game's own here.
float ps_depth(Vertex input) : SV_Target { return depth.Load(int3(int2(input.position.xy), 0)); }

Vertex vs_main(uint id : SV_VertexID) {
    Vertex v;
    const float2 uv = float2((id << 1) & 2, id & 2);
    v.position = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return v;
}

float depth_at(int2 p) {
    p = clamp(p, int2(0, 0), int2(up_width.w, back_height.w) - 1);
    return depth.Load(int3(p, 0));
}

// The ray through a pixel, in the world (not of unit length: one along the view).
float3 ray_of(float2 p) {
    const float a = (p.x + 0.5 - up_width.w * 0.5) / right_focal.w;
    const float b = (back_height.w * 0.5 - (p.y + 0.5)) / right_focal.w;
    return right_focal.xyz * a + up_width.xyz * b - back_height.xyz;
}

// The point the pixel shows, in the world. Reversed, infinite depth: distance = near / depth.
float3 world_at(int2 p, float d, float near_distance) {
    return eye_near.xyz + ray_of(p) * (near_distance / max(d, 1e-7));
}

// The near distance the game's depth is over, measured on the first place kept clear (the skater): its
// distance along the view, times the depth drawn where it shows on screen (depth = near / distance).
// Where its foot shows first; where its middle shows (a metre up, its surface a little nearer) when the
// foot is off screen, as in a close view. 0 when neither shows. The camera's own near plane is not
// known, and without it distances are only right in proportion.
float measure_with(float3 right, float3 up, float3 back, float3 eye, float3 target, float nearer) {
    const float3 to = target - eye;
    const float along_view = dot(to, -back);
    if (along_view < 0.3) return 0;
    const float2 at = float2(up_width.w * 0.5 + dot(to, right) * right_focal.w / along_view,
                             back_height.w * 0.5 - dot(to, up) * right_focal.w / along_view);
    if (any(at < 0) || at.x >= up_width.w || at.y >= back_height.w) return 0;
    const float there = depth_at(int2(at));
    return there > 0 ? (along_view - nearer) * there : 0;
}
float measure_at(float3 target, float nearer) {
    return measure_with(right_focal.xyz, up_width.xyz, back_height.xyz, eye_near.xyz, target, nearer);
}
float measured_near() {
    if (clear_info.x < 1) return 0;
    const float foot = measure_at(clears[0].xyz, 0);
    return foot > 0 ? foot : measure_at(clears[0].xyz + float3(0, 1, 0), 0.12);
}

// Measurements for world_cover.cpp to hold the near distance by (a 32 x 1 target, read back by the CPU):
// each column a point up the skater's own body, from a little under their place to over their head, a
// few centimetres either side of its middle. Each is the near distance that point says the depth is
// over (its surface taken as 0.1 m nearer than its middle), or 0 off screen. Those that show the skater
// agree; those that miss them show something further away. The CPU keeps the biggest group that agrees.
// The distance measured at one pixel jumped as the foot, board or shoe showed there, and the place
// kept clear slid about with it; points on the ground round the skater were no better:
// the skater's place is not on the ground, and they measured the air (2026-11-11).
float ps_measure(Vertex input) : SV_Target {
    const int2 p = int2(input.position.xy);
    if (clear_info.x < 1 || p.y >= CANDIDATES) return 0;
    const int at = p.y * 4;
    const uint k = (uint)p.x;
    const float side = (k < 16u ? -0.06 : 0.06);
    const float3 target = clears[0].xyz + float3(0, -0.4 + (k % 16u) * 0.13, 0) + cameras[at].xyz * side;
    return measure_with(cameras[at].xyz, cameras[at + 1].xyz, cameras[at + 2].xyz, cameras[at + 3].xyz, target, 0.1);
}

float hash(float2 p) {
    p = frac(p * float2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return frac(p.x * p.y);
}
float3 hash3(float3 p) {
    p = frac(p * float3(0.1031, 0.1030, 0.0973));
    p += dot(p, p.yxz + 33.33);
    return frac((p.xxy + p.yxx) * p.zyx);
}
// A cell's value, its coordinates wrapped every 289 cells first: world coordinates times the noise's
// scale reach the millions, where hash() has no fraction left and gave flat blocks with straight edges.
float cell_hash(float2 c) { return hash(c - 289.0 * floor(c / 289.0)); }
// How much a trail has pressed the cover down at the pixel: 0 none, 1 all of it. Where the pixel lies on
// the trail is found where its ray from the camera meets the ground at the trail's own height, which takes
// only the camera, not the depth's scale: from the depth, the trail slid over the ground whenever the scale
// measured changed (getting off the board, walking about, 2026-11-13). The depth only says whether the
// pixel is that ground at all (`here`, roughly), not a rail or a leg over it.
// `shade`: how grey the pressed snow is at the pixel against the trail's own (1). Where a point has a tread
// (its second float4's y, the tread's width), the tread's 13 lines run along the trail across that width,
// centred on it: 7 lighter (0.55), with the tread's 6 raised ridges between them pressed darker (1.3), the
// ridges a little narrower; outside the tread, and where the lines would be finer than the pixels, 1.
float trail_at(float3 here, float3 dir, out float shade) {
    shade = 1;
    const float4 head = cameras[TRAIL];
    const int points = min((int)head.x, MOST_TRAIL);
    if (points < 2) return 0;
    const float4 box = cameras[TRAIL + 1];
    if (here.x < head.y - 1 || here.z < head.z - 1 || here.x > head.w + 1 || here.z > box.x + 1 || here.y < box.y - 1 || here.y > box.z + 1)
        return 0;
    if (dir.y >= -1e-4) return 0; // a ray not going down meets no ground
    float pressed = 0;
    float4 a = cameras[TRAIL + 2];
    float strength_a = cameras[TRAIL + 3].x;
    float lines_strength = 0, lines_at = 0;
    [loop] for (int i = 1; i < points; ++i) {
        const float4 b = cameras[TRAIL + 2 + i * 2];
        const float4 more_b = cameras[TRAIL + 3 + i * 2];
        const float strength_b = more_b.x, tread = more_b.y;
        const bool starts = more_b.z > 0.5; // b begins a new trail: no stretch from a
        const float2 along = b.xz - a.xz;
        const float length_squared = dot(along, along);
        // Two points far apart are not one trail (a teleport, a respawn), nor is a point that starts a new one.
        // Far from this stretch (from the point the depth gives, on the ground): none of the work below. Every
        // pixel went through all of it for every point (2026-11-14).
        const float2 from_a = here.xz - a.xz;
        const float2 nearest = from_a - along * saturate(dot(from_a, along) / max(length_squared, 1e-6));
        const float reach_out = b.w * 0.5 + 0.5;
        if (!starts && length_squared < 4.0 && length_squared > 1e-6 && strength_a > 0 && strength_b > 0 &&
            dot(nearest, nearest) < reach_out * reach_out) {
            // The ray onto the ground of this stretch: the plane through both its points, level across it, so it
            // follows a slope along the trail. A level plane at the stretch's middle height drew a trail up a
            // slope as steps, jumping from one to the next (2026-11-13).
            const float3 sideways = normalize(float3(-along.y, 0, along.x));
            float3 ground_up = normalize(cross(b.xyz - a.xyz, sideways));
            if (ground_up.y < 0) ground_up = -ground_up;
            const float facing_ground = dot(dir, ground_up);
            const float reach = facing_ground < -1e-4 ? dot(a.xyz - eye_near.xyz, ground_up) / facing_ground : -1;
            const float3 on = eye_near.xyz + dir * reach;
            const float seen = length(here - eye_near.xyz);
            const float t = saturate(dot(on.xz - a.xz, along) / max(length_squared, 1e-6));
            const float2 to = on.xz - (a.xz + along * t);
            const float off = length(to);
            if (reach > 0 && abs(here.y - lerp(a.y, b.y, t)) < 0.4 && abs(reach - seen) < seen * 0.15 + 0.1) {
                const float half_width = b.w * 0.5;
                const float here_pressed = (1 - smoothstep(half_width * 0.6, half_width, off)) * lerp(strength_a, strength_b, t);
                if (here_pressed > pressed) {
                    pressed = here_pressed;
                    lines_strength = 0;
                    if (tread > 0) {
                        // Across the trail, signed, in pairs of lines (a light one and a ridge): the middle of a
                        // light one at the trail's middle, 13 lines across the tread's width.
                        const float pair = tread / 6.5;
                        const float across = (along.x * to.y - along.y * to.x) * rsqrt(max(length_squared, 1e-6));
                        const float f = frac(across / pair + 0.5);
                        // A ridge where f is far from a light line's middle (0.5): 44% of each pair.
                        lines_at = smoothstep(0.26, 0.3, abs(f - 0.5));
                        // A pixel this far off covers reach / focal metres: none finer than about two of them,
                        // and none past the tread's edge.
                        const float pixel = reach / right_focal.w;
                        lines_strength = (1 - smoothstep(pair * 0.22, pair * 0.4, pixel)) * (1 - smoothstep(tread * 0.5, tread * 0.5 + 0.005, abs(across)));
                    }
                }
            }
        }
        a = b;
        strength_a = strength_b;
    }
    shade = lerp(1, lerp(0.55, 1.3, lines_at), lines_strength);
    return pressed;
}

float noise(float2 p) {
    const float2 i = floor(p), f = frac(p);
    const float2 u = f * f * (3 - 2 * f);
    return lerp(lerp(cell_hash(i), cell_hash(i + float2(1, 0)), u.x), lerp(cell_hash(i + float2(0, 1)), cell_hash(i + float2(1, 1)), u.x), u.y);
}

// One grid of the roof map round a point: how much it is under a roof (0 to 1, the four nearest cells' middles
// blended), or -1 where one of them is not measured, or measured for another place. Loops, not unrolled: the
// shader compiled for 25 s with the roof checks written out (2026-11-14).
float roof_grid(float3 at, int base, int n, float size, float clearance) {
    const float2 c = at.xz / size - 0.5;
    const float2 corner = floor(c);
    const float2 f = c - corner;
    float under = 0;
    [loop] for (int k = 0; k < 4; ++k) {
        const int2 which = int2(k & 1, k >> 1);
        const int2 cell = int2(corner) + which;
        const int2 slot = ((cell % n) + n) % n;
        const float4 v = cameras[base + slot.x + slot.y * n];
        if (!(v.w > 0 && (int)v.y == cell.x && (int)v.z == cell.y)) return -1;
        const float weight = (which.x ? f.x : 1 - f.x) * (which.y ? f.y : 1 - f.y);
        under += (at.y < v.x - clearance ? 1 : 0) * weight;
    }
    return under;
}
// How much a point is under a roof (0 to 1): from the fine near grid, else the coarse far one, else none.
// `clearance`: how far over the point something must be to count (a bench or a rail on the ground does not).
float roof_blend(float3 at, float clearance) {
    const float4 head = cameras[ROOF];
    if (head.x <= 0) return 0;
    const int near_cells = (int)head.y;
    float r = roof_grid(at, ROOF + 2, near_cells, head.z, clearance);
    if (r < 0) r = roof_grid(at, ROOF + 2 + near_cells * near_cells, (int)head.w, cameras[ROOF + 1].x, clearance);
    return max(r, 0);
}
// The cover's edge under a roof, as snow drifts in under one: the roof map averaged over a disc about 4.5 m
// across, so the snow carries on a metre or so in under the edge and thins out smoothly over about two
// metres, all gone further in. A broad, gentle wander in the world keeps the edge from looking ruled, and
// stays put. (Before: an edge a few centimetres, then one of 1.5 m with small, jagged noise, 2026-11-13.)
float roofed_at(float3 at) {
    // No roof near, from one read (most of the ground): the highest surface within 3.5 m of the point's cell,
    // where its cell is in the near grid as that was worked out (world_cover.cpp, roof_near_reach).
    const float4 head = cameras[ROOF];
    if (head.x <= 0) return 0;
    const int near_cells = (int)head.y;
    const int2 cell = int2(floor(at.xz / head.z));
    const int2 first = int2(cameras[ROOF + 1].yz);
    if (all(cell >= first) && all(cell < first + near_cells)) {
        const int2 slot = ((cell % near_cells) + near_cells) % near_cells;
        const int index = slot.x + slot.y * near_cells;
        const float4 tops = cameras[ROOF + 2 + near_cells * near_cells + (int)head.w * (int)head.w + index / 4];
        const int which = index % 4;
        const float near_top = which == 0 ? tops.x : which == 1 ? tops.y : which == 2 ? tops.z : tops.w;
        if (at.y >= near_top - ROOF_CLEARANCE) return 0;
    }
    const float centre = roof_blend(at, ROOF_CLEARANCE);
    float outer = 0;
    [loop] for (int k = 0; k < 6; ++k) {
        const float angle = k * 1.0471976;
        outer += roof_blend(at + float3(cos(angle), 0, sin(angle)) * 2.2, ROOF_CLEARANCE);
    }
    if (centre + outer <= 0) return 0; // no roof near: most of the ground
    float inner = 0;
    [loop] for (int j = 0; j < 6; ++j) {
        const float angle = j * 1.0471976 + 0.5235988;
        inner += roof_blend(at + float3(cos(angle), 0, sin(angle)) * 1.1, ROOF_CLEARANCE);
    }
    float c = (centre * 2 + inner + outer) / 14;
    c += (noise(at.xz * 0.5) - 0.5) * 0.12;
    return smoothstep(0.5, 0.97, c);
}
// Whether a falling flake is under a roof. Flakes are hidden there one by one, so indoors none fall, and
// through a doorway it still snows outside.
bool roof_over(float3 at) { return roof_blend(at, 0) > 0.5; }

float luminance(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

// The middle brightness of 16 samples of the picture round p, on the same plane as `here` (within
// `flat` metres of it along `normal`), reaching up to `reach` pixels, turned by `spin`: `fallback`
// when fewer than four are on the plane.
float median_light(int2 p, float3 here, float3 normal, float near_distance, float reach, float flat, float spin, float fallback) {
    const int2 size = int2(up_width.w, back_height.w);
    float v[16];
    int found = 0;
    [unroll] for (int i = 0; i < 16; ++i) {
        const float angle = spin + i * 2.3999632; // golden angle: spread evenly, no two in line
        const float radius = reach * sqrt((i + 0.5) / 16.0);
        const int2 at = clamp(p + int2(float2(cos(angle), sin(angle)) * radius), int2(0, 0), size - 1);
        const float there = depth_at(at);
        const bool same = there > 0 && abs(dot(world_at(at, there, near_distance) - here, normal)) <= flat;
        v[i] = same ? luminance(scene.Load(int3(at, 0)).rgb) : -1.0;
        found += same ? 1 : 0;
    }
    if (found < 4) return fallback;
    // Sorted (16 is few): those off the plane (-1) first, then the middle of the rest. Each pass a fixed
    // length: Proton's shader compiler (Wine's) could not unroll an inner loop that starts at the outer's
    // count, and the cover never came on there (Steam Deck, 2026-11-13).
    [unroll] for (int a = 1; a < 16; ++a) {
        [unroll] for (int b = 15; b > 0; --b) {
            if (b <= a) {
                const float lo = min(v[b - 1], v[b]), hi = max(v[b - 1], v[b]);
                v[b - 1] = lo;
                v[b] = hi;
            }
        }
    }
    return v[16 - found + found / 2];
}

// Snow falling, in the world: a grid of cells over the world that falls and drifts with the wind, each
// cell holding one flake or none; every pixel steps along its ray through the cells it crosses, as far
// as the scene (nearer flakes only), and draws the flakes it passes close to. So they keep their places
// in the world: running forward, they come at the camera and pass it. Each is drawn out along how far
// it moved against the camera over a frame (its fall, the wind, the camera's own move), the streak a
// moving flake makes. Flakes placed by direction alone, round the camera, moved with it and looked like
// a picture laid over the screen (2026-11-11).
float flake_layer(float3 dir, float limit, float cell, float chance, float radius, float salt, float3 shift, float3 streak, float seconds, float top) {
    const float3 o = (eye_near.xyz + shift) / cell; // the camera, in this layer's cells
    float3 c = floor(o);
    const float3 way = float3(dir.x >= 0 ? 1 : -1, dir.y >= 0 ? 1 : -1, dir.z >= 0 ? 1 : -1);
    const float3 inv = 1.0 / max(abs(dir), 1e-5);
    float3 next = (c + max(way, 0) - o) * way * inv; // along the ray (in cells) to each axis's next wall
    const float streak_length = length(streak);
    const float focal = right_focal.w;
    float t = 0, seen = 0;
    [loop] for (int i = 0; i < 36; ++i) {
        if (t * cell > limit) break;
        const float3 seed = hash3(c + salt);
        if (seed.x < chance) {
            float3 centre = c + 0.2 + seed.yzx * 0.6;
            centre.xz += float2(sin(seconds * (0.6 + seed.y) + seed.z * 6.2831853), cos(seconds * (0.5 + seed.z) + seed.y * 6.2831853)) * (0.06 / cell);
            const float3 a = centre * cell - shift - eye_near.xyz; // the flake against the camera, in metres
            if (a.y <= top && !roof_over(a + eye_near.xyz)) { // above where the falling snow ends now, none falls any more; none under a roof
            // The nearest the ray comes to the flake's streak (a to a + streak).
            const float3 d = streak;
            const float dd = dot(d, d);
            const float rd = dot(dir, d);
            const float ra = dot(dir, a), da = dot(d, a);
            const float denominator = dd - rd * rd;
            float u = denominator > 1e-8 ? saturate((ra * rd - da) / denominator) : 0;
            const float3 on = a + d * u;
            const float along = dot(on, dir);
            if (along > 0.3 && along < limit) {
                const float off = length(on - dir * along) * focal / along;   // pixels from the ray
                const float size = radius * focal / along;                   // the flake's radius in pixels
                const float shown = max(size, 0.9);                           // at least about a pixel, fainter
                float touch = 1 - smoothstep(shown * 0.4, shown + 0.6, off);
                touch *= min(size / shown, 1.0) * (1.0 / (1.0 + streak_length * focal / along / (4.0 * shown)));
                touch *= 1 - smoothstep(limit * 0.7, limit, along);           // fading out with distance
                seen = max(seen, touch);
            }
            }
        }
        // On to the next cell along the ray.
        if (next.x < next.y && next.x < next.z) { t = next.x; next.x += inv.x; c.x += way.x; }
        else if (next.y < next.z) { t = next.y; next.y += inv.y; c.y += way.y; }
        else { t = next.z; next.z += inv.z; c.z += way.z; }
    }
    return seen;
}

float3 snowfall(float3 colour, int2 p, float scene_distance, float exposure) {
    const float amount = extra.z;
    if (amount <= 0) return colour;
    const float3 dir = normalize(ray_of(p));
    const float seconds = extra.y; // fallen, in seconds at the usual speed; the flakes' sway goes with it
    const float fall = 1.1 * max(extra.w, 0.05);          // metres a second down (about 1.1 at the usual speed)
    const float3 wind = float3(0.35, 0, 0.2) * max(extra.w, 0.3);
    const float3 velocity = float3(wind.x, -fall, wind.z);
    // The grid moves against the flakes: a flake fixed in the grid falls and drifts in the world.
    // How far it has fallen, kept by the CPU at the speed of each moment (extra.y): from the time times
    // this moment's speed, a change of speed moved every flake at once (2026-11-12). The wind goes with
    // the speed, as it does above (but for its floor at the slowest speeds).
    const float3 shift = -float3(0.35, -1.1, 0.2) * seconds;
    // Its streak over a frame, against the camera: back where it was, seen from where the camera was.
    float3 moved = -velocity / 60.0;
    if (last_right.w > 0) moved += eye_near.xyz - last_eye.xyz;
    const float moved_length = length(moved);
    if (moved_length > 0.4) moved *= 0.4 / moved_length;    // a jump (a cut, a respawn) is not a streak
    // Stopping: no new flakes; the highest one left falls at the flakes' speed from 12 m over the camera,
    // so the last ones fall out of the sky instead of all going at once.
    const float top = last_eye.w < 0 ? 1e6 : 12.0 - fall * last_eye.w;
    if (top < -30.0) return colour;
    const float near_limit = min(scene_distance, 7.0);
    const float far_limit = min(scene_distance, 22.0);
    float flakes = flake_layer(dir, near_limit, 0.45, amount * 0.45, 0.006, 0.5, shift, moved, seconds, top);
    flakes = max(flakes, flake_layer(dir, far_limit, 1.3, amount * 0.6, 0.012, 17.5, shift, moved, seconds, top) * 0.8);
    const float3 white = colour_amount.rgb * exposure * 1.6;
    return lerp(colour, white, saturate(flakes) * 0.85);
}

// The picture's brightness as a whole, for the flakes: a few points over the screen.
float exposure_now() {
    float sum = 0;
    [unroll] for (int y = 1; y <= 3; ++y)
        [unroll] for (int x = 1; x <= 3; ++x)
            sum += luminance(scene.Load(int3(int2(up_width.w * x / 4, back_height.w * y / 4), 0)).rgb);
    return max(sum / 9, 1e-4);
}

Output ps_main(Vertex input) {
    Output out_;
    const int2 p = int2(input.position.xy);
    const float4 under = scene.Load(int3(p, 0));
    out_.colour = under;
    const float d = depth_at(p);
    // The near distance the CPU holds from the measurements of many frames (eye_near.w), else this
    // frame's own from the skater's foot; 0 when neither is known.
    const float near_measured = eye_near.w > 0 ? eye_near.w : measured_near();
    const float near_distance = near_measured > 0 ? near_measured : 0.1;
    const float exposure = exposure_now();
    // The sky, and anything at the near plane (the camera inside something): snow falls in front of it.
    if (!(d > 0.0 && d < 1.0)) {
        out_.colour.rgb = snowfall(under.rgb, p, d >= 1.0 ? 0 : 1e9, exposure);
        return out_;
    }
    const float3 here = world_at(p, d, near_distance);
    const float view_distance = dot(here - eye_near.xyz, -back_height.xyz);
    const float scene_distance = length(here - eye_near.xyz);
    // The patchiness, where the point really is: the near distance held from many frames (eye_near.w),
    // not this frame's own, which came and went with the skater's feet and made the snow flash
    // (2026-11-10). A fixed guess put the pattern at the wrong distance, and it slid over the ground as
    // the camera moved, the melt's patches with it (2026-11-12). The guess only until one is held.
    const float3 steady = world_at(p, d, eye_near.w > 0 ? eye_near.w : 0.1);
    // The surface's direction from its neighbours: on each axis the one nearer in depth, so an edge
    // against something further away does not bend it.
    const float dl = depth_at(p - int2(1, 0)), dr = depth_at(p + int2(1, 0));
    const float du = depth_at(p - int2(0, 1)), dd = depth_at(p + int2(0, 1));
    const bool use_right = abs(dr - d) < abs(dl - d), use_down = abs(dd - d) < abs(du - d);
    const int2 px = use_right ? p + int2(1, 0) : p - int2(1, 0);
    const int2 py = use_down ? p + int2(0, 1) : p - int2(0, 1);
    float3 across = world_at(px, use_right ? dr : dl, near_distance) - here;
    float3 along = world_at(py, use_down ? dd : du, near_distance) - here;
    if (!use_right) across = -across;
    if (!use_down) along = -along;
    float3 normal = cross(along, across);
    const float length_squared = dot(normal, normal);
    float cover = 0;
    const float3 shown = under.rgb;
    if (length_squared > 1e-20) {
        normal *= rsqrt(length_squared);
        // Facing the camera: the side the pixel is seen from.
        if (dot(normal, eye_near.xyz - here) < 0) normal = -normal;
        // The surface's direction from a wider look too, points 12 pixels round, where that is the same surface
        // (its direction within about 25 degrees of the near one's, blended in from about 35): a curved ramp is
        // made of flat strips, and with each strip's own direction the snow lay in steps up it, a strip at a time
        // (2026-11-14). Across an edge the two disagree, and it stays the near one's.
        {
            const int2 wx = int2(12, 0), wy = int2(0, 12);
            const float3 wide_across = world_at(p + wx, depth_at(p + wx), near_distance) - world_at(p - wx, depth_at(p - wx), near_distance);
            const float3 wide_along = world_at(p + wy, depth_at(p + wy), near_distance) - world_at(p - wy, depth_at(p - wy), near_distance);
            float3 wide = cross(wide_along, wide_across);
            const float wide_squared = dot(wide, wide);
            if (wide_squared > 1e-20) {
                wide *= rsqrt(wide_squared);
                if (dot(wide, eye_near.xyz - here) < 0) wide = -wide;
                normal = normalize(lerp(normal, wide, smoothstep(0.82, 0.9, dot(wide, normal))));
            }
        }

        // Patchy where it thins: large, slow noise over the ground, in the world, so it stays put.
        const float patch = noise(steady.xz * 0.45) * 0.65 + noise(steady.xz * 1.9) * 0.35;
        const float facing = shape.x + (patch - 0.5) * shape.w * 0.6;
        const float softness = max(shape.y, 0.01);
        cover = smoothstep(facing - softness, facing + softness, normal.y) * colour_amount.w;
        // Only on surfaces flat across their neighbours, each side apart, flat if either is. A plane's
        // reversed depth changes evenly across the screen, so its second difference is nothing; leaves,
        // wires and distant edges are not flat, and the cover flickered on them. Next to an edge one
        // side is the other surface, and judged together the ground there had a dark rim (2026-11-11).
        const float dl2 = depth_at(p - int2(2, 0)), dr2 = depth_at(p + int2(2, 0));
        const float du2 = depth_at(p - int2(0, 2)), dd2 = depth_at(p + int2(0, 2));
        const float bend_across = min(abs(dl2 - 2 * dl + d), abs(d - 2 * dr + dr2));
        const float bend_along = min(abs(du2 - 2 * du + d), abs(d - 2 * dd + dd2));
        // And only where it bends sharply for how steeply it already slopes on the screen: where a ramp's flat
        // strips meet, a little, it is still flat; leaves, wires and edges bend sharply. With the bend alone
        // the snow thinned along every join up a ramp (2026-11-14).
        const float slope_across = max(abs(dr - dl) * 0.5, d * 1e-4), slope_along = max(abs(dd - du) * 0.5, d * 1e-4);
        const float sharp = max(bend_across / slope_across, bend_along / slope_along);
        cover *= 1 - smoothstep(0.0015, 0.006, max(bend_across, bend_along) / d) * smoothstep(0.2, 0.5, sharp);
        // None under a roof (indoors, under a bridge), where the plugin asks.
        cover *= 1 - roofed_at(steady);
    }

    // Distances are only real when measured: the trails and the places kept clear need them.
    float pressed = 0, shade = 1;
    bool dusted = false; // a place kept clear shows some snow on what stands in it
    [unroll] for (int k = 0; k < 3; ++k) dusted = dusted || (k < (int)clear_info.x && cameras[PLACES + k * 3].x > 0);
    const float untrailed = cover; // before any trail pressed it (a board-shaped place has none)
    if (near_measured > 0 && cover > 0.002) {
        // Trails: where a board went or a skater rolled, the snow packed down, thinner and greyer, filling
        // back in as the trail fades.
        pressed = trail_at(steady, normalize(ray_of(p)), shade);
        cover *= 1 - pressed * 0.35;
    }
    if (near_measured > 0 && (cover > 0.002 || dusted)) {
        // The places kept clear (the skater): upright cylinders over their feet, softened at the edge.
        // Inside one, the ground keeps its snow and whatever stands on it does not: the ground is the
        // lowest surface in a small ring round the pixel, and a point more than a centimetre or so over
        // it is something on it (the deck, drawn almost on the ground, had a white rim of snow;
        // clearing the ground too left a dark ring round the skater, 2026-11-10).
        [unroll] for (int c = 0; c < 3; ++c) {
            if (c >= (int)clear_info.x) break;
            const float3 from = here - clears[c].xyz;
            const float radius = clears[c].w;
            const float height = c == 0 ? clear_info.y : c == 1 ? clear_info.z : clear_info.w;
            const float snow = cameras[PLACES + c * 3].x;
            // A board-shaped place (a deck riding low, which the cylinder's ground test takes for the
            // ground): in its own plane and outline, rounded at its ends, the cover is `snow` of what it would
            // be and no trail is pressed into it. A trail under the deck showed on its griptape (2026-11-13).
            const float4 axis = cameras[PLACES + c * 3 + 1], side_axis = cameras[PLACES + c * 3 + 2];
            if (axis.w > 0) {
                // Where the pixel's ray meets the place's own plane, which takes only the camera: from the
                // depth, whose scale is measured, the deck's pixels fell either side of a plane a few
                // centimetres thick, and its own track showed on its griptape (2026-11-13). The depth only
                // says the pixel is about that far (the place, not the ground far past its edge).
                const float3 ray = normalize(ray_of(p));
                const float facing_place = dot(ray, side_axis.xyz);
                float in_board = 0;
                if (abs(facing_place) > 1e-3) {
                    const float reach = dot(clears[c].xyz - eye_near.xyz, side_axis.xyz) / facing_place;
                    const float3 met = eye_near.xyz + ray * reach - clears[c].xyz;
                    const float along = dot(met, axis.xyz);
                    const float across = length(met - axis.xyz * along - side_axis.xyz * dot(met, side_axis.xyz));
                    const float straight = max(axis.w - side_axis.w, 0);
                    const float edge = length(float2(along - clamp(along, -straight, straight), across));
                    const float seen = length(here - eye_near.xyz);
                    in_board = reach > 0 ? (1 - smoothstep(side_axis.w - 0.01, side_axis.w, edge)) *
                                           (1 - smoothstep(0.05, 0.1, abs(reach - seen) / max(seen, 0.1))) : 0;
                }
                pressed *= 1 - in_board;
                cover = lerp(cover, untrailed * snow, in_board);
                continue;
            }
            const float inside = 1 - smoothstep(radius * 0.75, radius, length(from.xz));
            if (inside <= 0 || from.y < -0.5 || from.y > height) continue;
            float ground = here.y, ring = 0;
            [unroll] for (int k = 0; k < 8; ++k) {
                const float angle = k * 0.7853982;
                const int2 at = p + int2(float2(cos(angle), sin(angle)) * max(back_height.w * 0.016, 6.0));
                const float there = depth_at(at);
                if (!(there > 0)) continue;
                const float3 point_there = world_at(at, there, near_distance);
                ground = min(ground, point_there.y);
                ring = max(ring, length(point_there.xz - here.xz));
            }
            // A slope rises across the ring too: allow for one of up to about 1 in 7.
            const float over = here.y - ground - max(0.005, ring * 0.12);
            // Well above the skater's own foot it is the skater, whatever is round it on screen: from a
            // low camera the shoulders and hair have only things further up or the sky round them, and
            // kept their snow (2026-11-11).
            // Only in the middle of the place (the body itself): across all of it, a ramp rising ahead of the
            // skater, and the ground before the camera when the distance measured was off, were taken for
            // the skater and cleared, a dark circle (2026-11-11).
            const float body = 1 - smoothstep(0.32, 0.42, length(from.xz));
            const float on_it = max(smoothstep(0, 0.004, over), smoothstep(0.55, 0.65, from.y) * body);
            // What stands in it has a patchy dusting of snow, on every side, where the plugin asks for some
            // (the skater after rolling in it); the pattern goes with the place, so it moves with them.
            const float dust = snow > 0 ? snow * smoothstep(0.35, 0.75, noise(from.xz * 14.0 + from.y * 9.0)) : 0;
            cover = lerp(cover, dust, inside * on_it);
            // And no trail on them: the trail's grey showed on the dusting on the skater's legs (2026-11-13).
            pressed *= 1 - inside * on_it;
        }
    }
    if (cover <= 0.002) {
        out_.colour.rgb = snowfall(shown, p, scene_distance, exposure);
        return out_;
    }

    // Lit as what it covers is lit, without its colour: so a shadow on the ground is a shadow on the
    // snow, and the game's exposure holds for both. Not from the pixel alone, or the ground's own
    // pattern (paving joints, cracks) shows through as grey: the middle (median) brightness of samples
    // round it on the same plane, close ones and, as a floor, wider ones. The brightest of a few samples
    // at fixed offsets copied anything bright near them onto the snow once for every offset (the deck,
    // shoes, a kerb: four to eight ghost copies round it, 2026-11-11); one thing cannot be the middle of
    // sixteen, and the samples turn a different way at every pixel, so nothing lines up into a copy.
    const float own = luminance(under.rgb);
    const float spin = hash(float2(p) * 0.618034) * 6.2831853; // the same each frame: no shimmer
    const float close = median_light(p, here, normal, near_distance, max(back_height.w * 0.008, 3.0), max(0.004, view_distance * 0.002), spin, own);
    const float wide = median_light(p, here, normal, near_distance, back_height.w * 0.03, max(0.02, view_distance * 0.01), spin + 0.2, close);
    // Not darker than about 60% of the snow round about: the picture still has the game's motion blur
    // and smear in it, faint dark copies of moving things (and the skater's soft contact shadow) that
    // showed on the snow as ghost outlines and a dark disc. Snow scatters light: its shadows are pale.
    const float light = max(lerp(close, own, 0.1), wide * 0.6);
    // Melting: the same everywhere, in patches, the thin places first, so it does not just fade out
    // evenly. Shade melting slower was judged from the picture, and roads, the player's shadow and a
    // change of weather broke it (2026-11-12).
    const float left = last_back.w;
    if (left < 0.999) {
        // None at all once it has all melted (left 0), all of it while none has (left 1).
        const float thin = noise(steady.xz * 0.9) * 0.6 + noise(steady.xz * 2.1) * 0.2;
        cover *= smoothstep(thin, thin + 0.35, left * 1.15);
        if (cover <= 0.002) {
            out_.colour.rgb = snowfall(shown, p, scene_distance, exposure);
            return out_;
        }
    }
    // Pressed snow is greyer: a tread's light lines a little less, its ridges a little more.
    float3 lit = colour_amount.rgb * light * shape.z * (1 - saturate(pressed * 0.3 * shade));
    if (extra.x > 0) {
        const float glint = cell_hash(floor(steady.xz * 38.0) + floor(steady.y * 38.0));
        lit += light * extra.x * 6.0 * step(0.996, glint);
    }
    out_.colour = float4(snowfall(lerp(shown, lit, cover), p, scene_distance, exposure), under.a);
    return out_;
}
