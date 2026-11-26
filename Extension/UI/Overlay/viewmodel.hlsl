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
    float4 bones[256 * 3];
};
cbuffer object : register(b2) { float4 object_transform[3]; };
cbuffer material : register(b0) {
    float4 layer_settings[3];
    float4 surface_default; // x roughness, y specular, z has pictures (1 colour, 2 normal, 4 surface), w opacity
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
    float4 colour : COLOR0;
    float2 uv1 : TEXCOORD1;
    float2 uv2 : TEXCOORD2;
};
struct ps_in {
    float4 clip : SV_POSITION;
    float3 view : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float4 tangent : TEXCOORD2;
    float2 uv : TEXCOORD3;
    float4 colour : COLOR0;
    float4 extra_uv : TEXCOORD4;
    float3 vertex_light : TEXCOORD5;
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
    float3 position=row_mul(object_transform[0],object_transform[1],object_transform[2],float4(v.position,1));
    float3 a=object_transform[0].xyz,b=object_transform[1].xyz,c=object_transform[2].xyz;
    float determinant=dot(a,cross(b,c));
    float3 local_normal=abs(determinant)>1e-12 ? float3(dot(cross(b,c),v.normal),dot(cross(c,a),v.normal),dot(cross(a,b),v.normal))/determinant : v.normal;
    float3 local_tangent=row_mul(object_transform[0],object_transform[1],object_transform[2],float4(v.tangent.xyz,0));
    float3 skinned=row_mul(r0,r1,r2,float4(position,1));
    float3 normal=row_mul(r0,r1,r2,float4(local_normal,0));
    float3 tangent=row_mul(r0,r1,r2,float4(local_tangent,0));
    ps_in o;
    o.view = row_mul(place[0], place[1], place[2], float4(skinned, 1));
    o.normal = row_mul(place[0], place[1], place[2], float4(normal, 0));
    o.tangent = float4(row_mul(place[0], place[1], place[2], float4(tangent, 0)), v.tangent.w);
    o.uv = v.uv;
    o.extra_uv = float4(v.uv1,v.uv2);
    o.colour = v.colour;
    float3 unit_normal=o.normal/max(length(o.normal),1e-10);
    float3 vertex_ambient=lerp(ground.rgb,sky.rgb,unit_normal.y*.5+.5)*light_colour.w;
    o.vertex_light=saturate(vertex_ambient+light_colour.rgb*saturate(dot(unit_normal,light.xyz)));
    o.clip = float4(o.view.x * lens.x, o.view.y * lens.y, o.view.z * lens.z + lens.w, o.view.z);
    return o;
}

float3 linear_to_srgb(float3 c) { return lerp(c * 12.92, 1.055 * pow(abs(c), 1.0 / 2.4) - 0.055, step(0.0031308, c)); }

float3 srgb_to_linear(float3 c) { return lerp(c/12.92,pow(max((c+0.055)/1.055,0),2.4),step(0.04045,c)); }
float wrap_coord(float x,uint mode,float half_texel) {
    if (mode==0) return clamp(x,half_texel,1-half_texel);
    if (mode==2) return clamp(1-abs(frac(x*.5)*2-1),half_texel,1-half_texel);
    return x; // The wrap sampler preserves gradients across tile boundaries.
}
float2 layer_uv(float2 uv,uint flags,float3 n,float3 h,float2 size) {
    uint source=flags&3;
    if (source==1) uv=float2(.5+.5*n.x,.5-.5*n.y);
    if (source==2) {
        float3 right=cross(float3(0,1,0),h);
        if(dot(right,right)<.001) right=float3(1,0,0);
        right=normalize(right);float3 up=cross(h,right);
        uv=float2(.5+.5*dot(n,right),.5-.5*dot(n,up));
    }

    float2 half_texel=.5/max(size,1);
    return float2(wrap_coord(uv.x,(flags>>2)&3,half_texel.x),wrap_coord(uv.y,(flags>>4)&3,half_texel.y));
}
float4 combine_layer(float4 prior,float4 tex,float4 raster,float4 settings) {
    uint flags=(uint)settings.x,mode=(uint)settings.y;
    if ((flags&3)!=0) {
        uint rgb=(uint)settings.w;
        float3 tint=float3((rgb>>16)&255,(rgb>>8)&255,rgb&255)/255;
        prior.rgb+=tex.rgb*settings.z*tint;return saturate(prior);
    }
    if (flags&128) {
        // Mask channels are authored numeric coverage, rather than linear light.
        float4 channels=float4(linear_to_srgb(tex.rgb),tex.a);
        prior.a*=channels[(flags>>8)&3];return prior;
    }
    uint colour=mode&15,alpha_mode=(mode>>4)&15;
    if(colour==0) prior.rgb*=tex.rgb;
    else if(colour==1) prior.rgb+=tex.rgb;
    else if(colour==2) prior.rgb-=tex.rgb;
    else if(colour==4) prior.rgb=lerp(prior.rgb,tex.rgb*raster.rgb,tex.a);
    if(alpha_mode==0) prior.a*=tex.a;
    else if(alpha_mode==1) prior.a+=tex.a;
    else if(alpha_mode==2) prior.a-=tex.a;
    else if(alpha_mode==4) prior.a=tex.a;
    else if(alpha_mode==5) prior.a=1;
    return saturate(prior);
}

float4 ps_main(ps_in p, bool front_face : SV_IsFrontFace) : SV_Target {
    uint has = (uint)surface_default.z;
    float4 albedo = (has&128) ? float4(1,1,1,1) : ((has & 1) ? albedo_map.Sample(sampler0, p.uv) : float4(0.6, 0.6, 0.6, 1));
    albedo *= p.colour;
    // Cutout textures (eyelashes, leaves and similar cards) must not fill their transparent area.
    float alpha = albedo.a * surface_default.w;
    if (has & 1024) clip(albedo.a - (surface_default.w < 1 ? 0.003 : 0.5));
    float3 base = albedo.rgb; // an sRGB view: linear
    float3 n = normalize(p.normal);
    if ((has & 2) && !(has&128)) {
        float3 t = normalize(p.tangent.xyz - n * dot(n, p.tangent.xyz));
        float3 b = cross(n, t) * p.tangent.w;
        float3 m = normal_map.Sample(sampler0, p.uv).xyz * 2 - 1;
        n = normalize(t * m.x + b * m.y + n * m.z);
    }
    float roughness = surface_default.x, occlusion = 1, specular = surface_default.y;
    if ((has & 4) && !(has & (32|128))) {
        float3 s = surface_map.Sample(sampler0, p.uv).rgb;
        roughness = s.r;
        occlusion = s.g;
        specular = s.b;
    }
    float3 to_eye = normalize(-p.view);
    // Smooth authored normals may point past the eye at a grazing angle.
    // Flipping them per pixel makes fixed surfaces change lighting as the
    // camera moves. Only reverse a genuinely back-facing, two-sided primitive.
    if ((has & 1024) ? ((has & 256) && !front_face) : dot(n, to_eye) < 0) n = -n;
    float3 l = light.xyz;
    float lit = saturate(dot(n, l));
    float3 ambient = lerp(ground.rgb, sky.rgb, dot(n, float3(0,1,0)) * 0.5 + 0.5) * light_colour.w * occlusion;
    float3 h = normalize(l + to_eye);
    float shine = exp2(11.0 * (1.0 - saturate(roughness)) + 1.0);
    float highlight = (shine + 8.0) / 25.13 * pow(saturate(dot(n, h)), shine) * lit;
    float3 colour = base * (ambient + light_colour.rgb * lit) + light_colour.rgb * highlight * specular * 0.25;
    // Legacy raster colours and texture modulation operate in display codes,
    // without a view-dependent specular lobe or a per-fragment terminator.
    if(has&512) colour=srgb_to_linear(linear_to_srgb(base)*p.vertex_light);
    // Smooth transparent surfaces reflect more at grazing angles. The shell's
    // centre stays clear while its curved silhouette catches the environment.
    if (surface_default.w < 0.9 && roughness < 0.3 && specular > 0.7) {
        float fresnel = 0.04 + 0.96 * pow(1 - saturate(dot(n, to_eye)), 5);
        alpha = saturate(alpha * (0.4 + 2.0 * fresnel));
        colour += sky.rgb * fresnel * 0.65;
    }
    if (has & 64) colour=base;
    if(has&128) {
        float4 layered=float4(colour,p.colour.a);
        uint w,hgt;
        if(has&1) {albedo_map.GetDimensions(w,hgt);layered=combine_layer(layered,albedo_map.Sample(sampler0,layer_uv(p.uv,(uint)layer_settings[0].x,n,h,float2(w,hgt))),float4(colour,1),layer_settings[0]);}
        if(has&2) {normal_map.GetDimensions(w,hgt);layered=combine_layer(layered,normal_map.Sample(sampler0,layer_uv(p.extra_uv.xy,(uint)layer_settings[1].x,n,h,float2(w,hgt))),float4(colour,1),layer_settings[1]);}
        if(has&4) {surface_map.GetDimensions(w,hgt);layered=combine_layer(layered,surface_map.Sample(sampler0,layer_uv(p.extra_uv.zw,(uint)layer_settings[2].x,n,h,float2(w,hgt))),float4(colour,1),layer_settings[2]);}
        colour=layered.rgb;alpha=layered.a*surface_default.w;
        clip(layered.a-(surface_default.w<1?.003:.5));
    }
    if (has & 32) {
        // An environment matcap supplied by the model, oriented by light/eye.
        // It uses the source reflection texture rather than a flat glass tint.
        float3 right = cross(float3(0,1,0), h);
        if (dot(right,right) < 0.001) right = cross(float3(1,0,0),h);
        right = normalize(right);
        float3 up = cross(h,right);
        float2 uv = saturate(float2(0.5+0.5*dot(n,right),0.5-0.5*dot(n,up)));
        float reflection = surface_map.Sample(sampler0,uv).r * saturate(dot(n,h));
        float edge = pow(1-saturate(dot(n,to_eye)),3);
        colour = lerp(base, float3(1,1,1), reflection*0.9) + sky.rgb*edge*0.2;
        alpha = saturate(surface_default.w*(0.55+edge*0.45)+reflection*0.72*min(1,surface_default.w/0.24));
    }
    colour = max(colour * sky.w, 0);
    // The overlay's output, as ImGui's own (imgui_impl_dx12.cpp): sRGB codes in SDR, else linear light.
    if (output.x < 0.5) return float4(linear_to_srgb(saturate(colour)), alpha);
    colour = min(colour, 4.0);
    if (output.x < 1.5) return float4(colour * (output.y / 80.0), alpha);
    float3 wide = float3(dot(colour, float3(0.6274, 0.3293, 0.0433)), dot(colour, float3(0.0691, 0.9195, 0.0114)), dot(colour, float3(0.0164, 0.0880, 0.8956)));
    float3 y = pow(saturate(wide * (output.y / 10000.0)), 0.1593017578125);
    return float4(pow((0.8359375 + 18.8515625 * y) / (1.0 + 18.6875 * y), 78.84375), alpha);
}
