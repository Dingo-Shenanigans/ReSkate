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
    float4 ambient_up;  // world up resolved into the render-time view
    float4 contact_plane; // unit normal and signed plane distance, view space
    float4 contact_config; // x: contact margin; zero disables
    float4 scene_config; // enabled, point count, screen width, screen height
    float4 depth_points[8];
    float4 clip_circle; // absolute pixel centre/radius; zero radius disables
    float4 projection_shift; // NDC x/y shift, inset background alpha
    float4 fog; // start, end, curve, unused; zero curve disables
    float4 fog_colour;
    float4 reflection_plane;
    float4 reflection_config; // source time, distortion, reflection strength, pattern scale
    float4 reflection_tint;
    float4 draw_clip_plane;
    float4 bones[256 * 3];
};
cbuffer material : register(b0) {
    float4 surface_default; // x roughness, y specular, z has pictures (1 colour, 2 normal, 4 surface), w unused
    float4 layer_settings[3];
    float4 texture_scroll;
    float4 material_tint; // sRGB albedo multiplier, white when not specified
};
cbuffer object : register(b2) { float4 object_transform[3]; };
Texture2D<float> scene_map : register(t3);
Texture2D planar_reflection : register(t4);
Texture2D planar_transmission : register(t5);
Texture2D albedo_map : register(t0);
Texture2D normal_map : register(t1);
Texture2D surface_map : register(t2);
SamplerState sampler0 : register(s0);
SamplerState layer_sampler_0 : register(s1);
SamplerState layer_sampler_1 : register(s2);
SamplerState layer_sampler_2 : register(s3);
SamplerState layer_sampler_3 : register(s4);
SamplerState layer_sampler_4 : register(s5);
SamplerState layer_sampler_5 : register(s6);
SamplerState layer_sampler_6 : register(s7);
SamplerState layer_sampler_7 : register(s8);
SamplerState layer_sampler_8 : register(s9);

struct vs_in {
    float3 position : POSITION;
    float3 normal : NORMAL;
    float4 tangent : TANGENT;
    float2 uv : TEXCOORD0;
    float2 uv1 : TEXCOORD1;
    float2 uv2 : TEXCOORD2;
    uint4 joints : BLENDINDICES;
    float4 weights : BLENDWEIGHT;
    float4 colour : COLOR0;
};
struct ps_in {
    float4 clip : SV_POSITION;
    float3 view : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float4 tangent : TEXCOORD2;
    float2 uv : TEXCOORD3;
    nointerpolation float3 scene_projection : TEXCOORD4;
    float4 colour : COLOR0;
    float4 extra_uv : TEXCOORD5;
    float3 vertex_light : TEXCOORD6;
};

float3 row_mul(float4 r0, float4 r1, float4 r2, float4 p) { return float3(dot(r0, p), dot(r1, p), dot(r2, p)); }

// Fit d = a + b / z from collision-confirmed visible surface points. This handles
// normal and reversed depth without guessing the game's near or far clipping planes.
float3 scene_projection() {
    float inverse_z[8], depths[8];
    int count = (int)scene_config.y;
    if (count < 3) return 0;
    [unroll] for (int i = 0; i < 8; ++i) {
        float3 p = depth_points[i].xyz;
        inverse_z[i] = 1.0 / max(p.z, 0.01);
        float2 uv = float2(0.5 + 0.5 * p.x * lens.x / max(p.z, 0.01), 0.5 - 0.5 * p.y * lens.y / max(p.z, 0.01));
        depths[i] = scene_map.Load(int3(clamp(uv * scene_config.zw, 0, scene_config.zw - 1), 0));
    }
    float widest = 0; int first = 0, last = 1;
    [unroll] for (int j = 0; j < 8; ++j) [unroll] for (int k = j + 1; k < 8; ++k) {
        float spread = abs(inverse_z[j] - inverse_z[k]);
        if (j < count && k < count && spread > widest && depths[j] > 0 && depths[j] < 1 && depths[k] > 0 && depths[k] < 1) {
            widest = spread; first = j; last = k;
        }
    }
    if (widest < 0.005) return 0;
    float b = (depths[first] - depths[last]) / (inverse_z[first] - inverse_z[last]);
    float a = depths[first] - b * inverse_z[first];
    int valid = 0;
    [unroll] for (int n = 0; n < 8; ++n)
        if (n < count && abs(depths[n] - a - b * inverse_z[n]) < max(0.00002, abs(b * inverse_z[n]) * 0.025)) ++valid;
    return float3(a, b, valid >= 3 && abs(b) > 0.0001 ? 1 : 0);
}

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
    o.scene_projection = scene_config.x > 0.5 ? scene_projection() : float3(0, 0, 1);
    o.view = row_mul(place[0], place[1], place[2], float4(skinned, 1));
    o.normal = row_mul(place[0], place[1], place[2], float4(normal, 0));
    o.tangent = float4(row_mul(place[0], place[1], place[2], float4(tangent, 0)), v.tangent.w);
    o.uv = v.uv;
    o.extra_uv=float4(v.uv1,v.uv2);
    o.colour=v.colour;
    // Classic fixed-function lighting clamps at the vertex before interpolation.
    float3 unit_normal=o.normal/max(length(o.normal),1e-10);
    float3 vertex_ambient=lerp(ground.rgb,sky.rgb,dot(unit_normal,ambient_up.xyz)*0.5+0.5)*light_colour.w;
    o.vertex_light=saturate(vertex_ambient+light_colour.rgb*saturate(dot(unit_normal,light.xyz)));
    o.clip = float4(o.view.x * lens.x, o.view.y * lens.y, o.view.z * lens.z + lens.w, o.view.z);
    o.clip.xy += projection_shift.xy * o.clip.w;
    return o;
}

float3 linear_to_srgb(float3 c) { return lerp(c * 12.92, 1.055 * pow(abs(c), 1.0 / 2.4) - 0.055, step(0.0031308, c)); }
float3 srgb_to_linear(float3 c) { return lerp(c/12.92,pow(max((c+0.055)/1.055,0),2.4),step(0.04045,c)); }
float2 layer_uv(float2 uv,uint flags,float3 n,float3 h) {
    uint source=flags&3;
    if (source==1) uv=float2(.5+.5*n.x,.5-.5*n.y);
    if (source==2) {
        float3 right=cross(float3(0,1,0),h);
        if(dot(right,right)<.001) right=float3(1,0,0);
        right=normalize(right);float3 up=cross(h,right);
        uv=float2(.5+.5*dot(n,right),.5-.5*dot(n,up));
    }
    if(flags&64) uv+=texture_scroll.xy;
    return uv;
}
float4 sample_layer(Texture2D picture,float2 uv,uint flags) {
    // Hardware address modes retain continuous gradients across repeats and mirror folds.
    uint u=(flags>>2)&3, v=(flags>>4)&3;
    if(u==3) u=1;
    if(v==3) v=1;
    float2 dx=ddx(uv),dy=ddy(uv);
    switch(u*3+v) {
    case 0: return picture.SampleGrad(layer_sampler_0,uv,dx,dy);
    case 1: return picture.SampleGrad(layer_sampler_1,uv,dx,dy);
    case 2: return picture.SampleGrad(layer_sampler_2,uv,dx,dy);
    case 3: return picture.SampleGrad(layer_sampler_3,uv,dx,dy);
    case 4: return picture.SampleGrad(layer_sampler_4,uv,dx,dy);
    case 5: return picture.SampleGrad(layer_sampler_5,uv,dx,dy);
    case 6: return picture.SampleGrad(layer_sampler_6,uv,dx,dy);
    case 7: return picture.SampleGrad(layer_sampler_7,uv,dx,dy);
    default: return picture.SampleGrad(layer_sampler_8,uv,dx,dy);
    }
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

float4 write_output(float3 colour,float alpha) {
    colour = max(colour * sky.w, 0);
    // The overlay's output, as ImGui's own (imgui_impl_dx12.cpp): sRGB codes in SDR, else linear light.
    if (output.x < 0.5) return float4(linear_to_srgb(saturate(colour)), alpha);
    colour = min(colour, 4.0);
    if (output.x < 1.5) return float4(colour * (output.y / 80.0), alpha);
    float3 wide = float3(dot(colour, float3(0.6274, 0.3293, 0.0433)), dot(colour, float3(0.0691, 0.9195, 0.0114)), dot(colour, float3(0.0164, 0.0880, 0.8956)));
    float3 y = pow(saturate(wide * (output.y / 10000.0)), 0.1593017578125);
    return float4(pow((0.8359375 + 18.8515625 * y) / (1.0 + 18.6875 * y), 78.84375), alpha);
}

float4 ps_main(ps_in p, bool front_face : SV_IsFrontFace) : SV_Target {
    if (dot(draw_clip_plane.xyz,draw_clip_plane.xyz)>.5) clip(dot(draw_clip_plane.xyz,p.view)+draw_clip_plane.w);
    if (clip_circle.z>0 && dot((p.clip.xy-clip_circle.xy)/clip_circle.zw,(p.clip.xy-clip_circle.xy)/clip_circle.zw)>1) discard;
    // A missing depth sample cannot hide an otherwise valid model. The caller's
    // world visibility checks remain useful while a scene projection is unavailable.
    uint has = (uint)surface_default.z;
    if (scene_config.x > 0.5 && p.scene_projection.z > 0.5 && !((has & 16) && contact_config.y > 0.5)) {
        float d = scene_map.Load(int3(p.clip.xy, 0));
        float z = p.scene_projection.y / (d - p.scene_projection.x);
        // A small geometric bias keeps a decal resting on its receiving surface.
        float3 occluder = p.view * (z / p.view.z);
        bool contact = (has & 16) && contact_config.x > 0
            && abs(dot(contact_plane.xyz, occluder) + contact_plane.w) <= contact_config.x
            && abs(dot(contact_plane.xyz, p.view) + contact_plane.w) <= contact_config.x * 2 + 0.05;
        if (z > 0 && z < p.view.z - 0.018 && !contact) discard;
        if (((uint)surface_default.z & 8) && abs(z - p.view.z) > 0.06) discard;
    }
    if ((has & (2048u|4096u)) != 0) {
        float2 uv=float2(.5+.5*p.view.x*lens.x/max(p.view.z,.01),.5-.5*p.view.y*lens.y/max(p.view.z,.01));
        float3 scene=(has&2048u) ? planar_reflection.Sample(layer_sampler_0,saturate(uv)).rgb :
                                   planar_transmission.Sample(layer_sampler_0,saturate(uv)).rgb;
        return write_output(scene,1);
    }
    if ((has&1024) && reflection_tint.w>.5) {
        float t=reflection_config.x;
        float2 uv=p.uv*reflection_config.w;
        float2 a=normal_map.Sample(sampler0,uv*.35+float2(t*.013,-t*.047)).rg*2-1;
        float2 b=normal_map.Sample(sampler0,uv*1.2+float2(-t*.021,t*.071)).rg*2-1;
        float2 ripple=(a+b*.5)*reflection_config.y;
        float distance=dot(reflection_plane.xyz,p.view)+reflection_plane.w;
        float3 mirrored=p.view-2*distance*reflection_plane.xyz;
        mirrored.x=-mirrored.x;
        float2 reflect_uv=float2(.5+.5*mirrored.x*lens.x/max(mirrored.z,.01),.5-.5*mirrored.y*lens.y/max(mirrored.z,.01));
        float2 transmit_uv=float2(.5+.5*p.view.x*lens.x/max(p.view.z,.01),.5-.5*p.view.y*lens.y/max(p.view.z,.01));
        float3 reflected=planar_reflection.Sample(layer_sampler_0,saturate(reflect_uv+ripple)).rgb;
        float3 transmitted=planar_transmission.Sample(layer_sampler_0,saturate(transmit_uv+ripple*.6)).rgb;
        float3 n=normalize(p.normal+float3(a.x*.06,a.y*.01,b.y*.06));
        float facing=abs(dot(n,normalize(-p.view)));
        float fresnel=saturate(.08+reflection_config.z*pow(1-facing,3));
        float3 colour=lerp(transmitted*reflection_tint.rgb,reflected,fresnel);
        float highlight=surface_map.Sample(sampler0,uv*.7+float2(t*.006,-t*.02)).r;
        colour+=highlight*highlight*.025*light_colour.rgb;
        return write_output(colour,surface_default.w);
    }
    float4 albedo = (has&128) ? float4(1,1,1,1) : ((has & 1) ? albedo_map.Sample(sampler0, p.uv) : float4(0.6, 0.6, 0.6, 1));
    albedo *= p.colour;
    albedo.rgb *= srgb_to_linear(material_tint.rgb);
    // Cutout textures (eyelashes, leaves and similar cards) must not fill their transparent area.
    float alpha = albedo.a * surface_default.w;
    if (has & 8192) clip(albedo.a - (surface_default.w < 1 ? 0.003 : 0.5));
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
    if ((has & 8192) ? ((has & 256) && !front_face) : dot(n,to_eye)<0) n = -n;
    float3 l = light.xyz;
    float lit = saturate(dot(n, l));
    float3 ambient = lerp(ground.rgb, sky.rgb, dot(n, ambient_up.xyz) * 0.5 + 0.5) * light_colour.w * occlusion;
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
        if(has&1) layered=combine_layer(layered,sample_layer(albedo_map,layer_uv(p.uv,(uint)layer_settings[0].x,n,h),(uint)layer_settings[0].x),float4(colour,1),layer_settings[0]);
        if(has&2) layered=combine_layer(layered,sample_layer(normal_map,layer_uv(p.extra_uv.xy,(uint)layer_settings[1].x,n,h),(uint)layer_settings[1].x),float4(colour,1),layer_settings[1]);
        if(has&4) layered=combine_layer(layered,sample_layer(surface_map,layer_uv(p.extra_uv.zw,(uint)layer_settings[2].x,n,h),(uint)layer_settings[2].x),float4(colour,1),layer_settings[2]);
        colour=layered.rgb;alpha=layered.a*surface_default.w;
        clip(layered.a-(surface_default.w<1?.003:.5));
    }
    if (has & 32) {
        // An environment matcap supplied by the model, oriented by light/eye.
        // It uses the source reflection texture rather than a flat glass tint.
        float3 right = cross(ambient_up.xyz, h);
        if (dot(right,right) < 0.001) right = cross(float3(1,0,0),h);
        right = normalize(right);
        float3 up = cross(h,right);
        float2 uv = saturate(float2(0.5+0.5*dot(n,right),0.5-0.5*dot(n,up)));
        float reflection = surface_map.Sample(sampler0,uv).r * saturate(dot(n,h));
        float edge = pow(1-saturate(dot(n,to_eye)),3);
        colour = lerp(base, float3(1,1,1), reflection*0.9) + sky.rgb*edge*0.2;
        alpha = saturate(surface_default.w*(0.55+edge*0.45)+reflection*0.72*min(1,surface_default.w/0.24));
    }
    if (fog.z>0 && texture_scroll.z<0.5) {
        float f=saturate((p.view.z-fog.x)/max(0.00001,fog.y-fog.x));
        if (fog.z==4) f=1-exp2(-8*f);
        else if (fog.z==5) f=1-exp2(-8*f*f);
        else if (fog.z==6) f=exp2(-8*(1-f));
        else if (fog.z==7) f=exp2(-8*(1-f)*(1-f));
        colour=lerp(colour,srgb_to_linear(fog_colour.rgb),saturate(f));
    }
    return write_output(colour,alpha);
}

float4 inset_vs(uint vertex : SV_VertexID) : SV_Position {
    float2 corner=float2((vertex<<1)&2,vertex&2);
    return float4(corner*2-1,0,1);
}
float4 inset_ps(float4 pixel : SV_Position) : SV_Target {
    float2 local=(pixel.xy-clip_circle.xy)/clip_circle.zw;
    if (dot(local,local)>1) discard;
    return float4(0,0,0,projection_shift.z);
}
