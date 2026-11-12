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
float noise(float2 p) {
    const float2 i = floor(p), f = frac(p);
    const float2 u = f * f * (3 - 2 * f);
    return lerp(lerp(cell_hash(i), cell_hash(i + float2(1, 0)), u.x), lerp(cell_hash(i + float2(0, 1)), cell_hash(i + float2(1, 1)), u.x), u.y);
}

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
    // Sorted (16 is few): those off the plane (-1) first, then the middle of the rest.
    [unroll] for (int a = 1; a < 16; ++a) {
        [unroll] for (int b = a; b > 0; --b) {
            const float lo = min(v[b - 1], v[b]), hi = max(v[b - 1], v[b]);
            v[b - 1] = lo;
            v[b] = hi;
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
            if (a.y <= top) { // above where the falling snow ends now, none falls any more
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
        cover *= 1 - smoothstep(0.0015, 0.006, max(bend_across, bend_along) / d);
    }

    // Distances are only real when measured: the places kept clear need them.
    if (near_measured > 0 && cover > 0.002) {
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
            cover *= 1 - inside * on_it;
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
    float3 lit = colour_amount.rgb * light * shape.z;
    if (extra.x > 0) {
        const float glint = cell_hash(floor(steady.xz * 38.0) + floor(steady.y * 38.0));
        lit += light * extra.x * 6.0 * step(0.996, glint);
    }
    out_.colour = float4(snowfall(lerp(shown, lit, cover), p, scene_distance, exposure), under.a);
    return out_;
}
