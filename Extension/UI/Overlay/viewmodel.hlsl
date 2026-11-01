// A viewmodel (reskate.viewmodel; viewmodel.cpp): a skinned model in the view's space (x right, y up,
// z forward, metres from the eye), drawn over the game's finished frame with a depth buffer of its own,
// as shooters draw the player's arms and gun. Lit by one light and a sky/ground ambient, with the
// material's colour, normal and surface (roughness, occlusion, specular) pictures.

cbuffer frame : register(b1) {
    float4 lens;        // x, y: the projection's scale; z, w: depth = z * lens.z + lens.w over z
    float4 place[3];    // the plugin's placement of the whole model, rows of a 3x4
    float4 light;       // xyz toward the light (view space, unit length), w unused
    float4 light_colour;// rgb, w the ambient's strength
    float4 sky;         // rgb the ambient from above, w the brightness of the whole
    float4 ground;      // rgb the ambient from below, w unused
    float4 output;      // x the transfer (0 SDR, 1 scRGB, 2 HDR10), y white in nits
    float4 bones[128 * 3];
};
cbuffer material : register(b0) {
    float4 surface_default; // x roughness, y specular, z has pictures (1 colour, 2 normal, 4 surface), w unused
};
Texture2D albedo_map : register(t0);
Texture2D normal_map : register(t1);
Texture2D surface_map : register(t2);
SamplerState sampler0 : register(s0);

struct vs_in {
    float3 position : POSITION;
    float3 normal : NORMAL;
    float4 tangent : TANGENT;
    float2 uv : TEXCOORD0;
    uint4 joints : BLENDINDICES;
    float4 weights : BLENDWEIGHT;
};
struct ps_in {
    float4 clip : SV_POSITION;
    float3 view : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float4 tangent : TEXCOORD2;
    float2 uv : TEXCOORD3;
};

float3 row_mul(float4 r0, float4 r1, float4 r2, float4 p) { return float3(dot(r0, p), dot(r1, p), dot(r2, p)); }

ps_in vs_main(vs_in v) {
    float4 r0 = 0, r1 = 0, r2 = 0;
    [unroll] for (int i = 0; i < 4; ++i) {
        uint b = v.joints[i] * 3;
        r0 += bones[b] * v.weights[i];
        r1 += bones[b + 1] * v.weights[i];
        r2 += bones[b + 2] * v.weights[i];
    }
    float3 skinned = row_mul(r0, r1, r2, float4(v.position, 1));
    float3 normal = row_mul(r0, r1, r2, float4(v.normal, 0));
    float3 tangent = row_mul(r0, r1, r2, float4(v.tangent.xyz, 0));
    ps_in o;
    o.view = row_mul(place[0], place[1], place[2], float4(skinned, 1));
    o.normal = row_mul(place[0], place[1], place[2], float4(normal, 0));
    o.tangent = float4(row_mul(place[0], place[1], place[2], float4(tangent, 0)), v.tangent.w);
    o.uv = v.uv;
    o.clip = float4(o.view.x * lens.x, o.view.y * lens.y, o.view.z * lens.z + lens.w, o.view.z);
    return o;
}

float3 linear_to_srgb(float3 c) { return lerp(c * 12.92, 1.055 * pow(abs(c), 1.0 / 2.4) - 0.055, step(0.0031308, c)); }

float4 ps_main(ps_in p) : SV_Target {
    uint has = (uint)surface_default.z;
    float3 base = (has & 1) ? albedo_map.Sample(sampler0, p.uv).rgb : float3(0.6, 0.6, 0.6); // an sRGB view: linear
    float3 n = normalize(p.normal);
    if (has & 2) {
        float3 t = normalize(p.tangent.xyz - n * dot(n, p.tangent.xyz));
        float3 b = cross(n, t) * p.tangent.w;
        float3 m = normal_map.Sample(sampler0, p.uv).xyz * 2 - 1;
        n = normalize(t * m.x + b * m.y + n * m.z);
    }
    float roughness = surface_default.x, occlusion = 1, specular = surface_default.y;
    if (has & 4) {
        float3 s = surface_map.Sample(sampler0, p.uv).rgb;
        roughness = s.r;
        occlusion = s.g;
        specular = s.b;
    }
    float3 to_eye = normalize(-p.view);
    if (dot(n, to_eye) < 0) n = -n; // the inside of a face seen through a gap
    float3 l = light.xyz;
    float lit = saturate(dot(n, l));
    float3 ambient = lerp(ground.rgb, sky.rgb, n.y * 0.5 + 0.5) * light_colour.w * occlusion;
    float3 h = normalize(l + to_eye);
    float shine = exp2(11.0 * (1.0 - saturate(roughness)) + 1.0);
    float highlight = (shine + 8.0) / 25.13 * pow(saturate(dot(n, h)), shine) * lit;
    float3 colour = base * (ambient + light_colour.rgb * lit) + light_colour.rgb * highlight * specular * 0.25;
    colour = max(colour * sky.w, 0);
    // The overlay's output, as ImGui's own (imgui_impl_dx12.cpp): sRGB codes in SDR, else linear light.
    if (output.x < 0.5) return float4(linear_to_srgb(saturate(colour)), 1);
    colour = min(colour, 4.0);
    if (output.x < 1.5) return float4(colour * (output.y / 80.0), 1);
    float3 wide = float3(dot(colour, float3(0.6274, 0.3293, 0.0433)), dot(colour, float3(0.0691, 0.9195, 0.0114)), dot(colour, float3(0.0164, 0.0880, 0.8956)));
    float3 y = pow(saturate(wide * (output.y / 10000.0)), 0.1593017578125);
    return float4(pow((0.8359375 + 18.8515625 * y) / (1.0 + 18.6875 * y), 78.84375), 1);
}
