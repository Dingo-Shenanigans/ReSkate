// SDR scene transmission from frame-matched surface/depth and native lit print.
Texture2D<float4> Scene:register(t0);
Texture2D<float> RawDepth:register(t1);
Texture2D<float4> Position:register(t2);
Texture2D<float4> NormalThickness:register(t3);
Texture2D<float4> Print:register(t4);
Texture2D<uint4> Calibration:register(t5);
SamplerState ClampLinear:register(s0);
cbuffer Material:register(b0){float3 Absorption;float IOR;float MaximumOffset;float PigmentDepth;float CoatStrength;float ReflectionStrength;};
float4 vs_main(uint id:SV_VertexID):SV_Position {float2 uv=float2((id<<1)&2,id&2);return float4(uv*float2(2,-2)+float2(-1,1),0,1);}
float linear_depth(int2 pixel,float4 calibration,bool perspective){
 float d=RawDepth.Load(int3(pixel,0));float denominator=perspective?d-calibration.x:calibration.x;
 if(abs(denominator)<1e-12)return 10000;
 float z=perspective?calibration.y/denominator:(d-calibration.y)/denominator;
 return isfinite(z)&&z>0?min(z,10000):10000;
}
float3 decode(float3 c){return pow(max(c,0),2.2);}
float3 encode(float3 c){return pow(max(c,0),1/2.2);}
float4 ps_main(float4 screen:SV_Position):SV_Target {
 uint width,height;Scene.GetDimensions(width,height);float2 size=float2(width,height),uv=screen.xy/size;
 float4 raw=Scene.Load(int3(screen.xy,0)),p=Position.Load(int3(screen.xy,0));
 if(p.a<=0||p.z<=0||!all(isfinite(p)))return raw;
 float4 cal=asfloat(Calibration.Load(int3(screen.xy,0)));
 if(!all(isfinite(cal))||abs(cal.z)<.01||abs(cal.w)<.01||abs(cal.z)>10||abs(cal.w)>10)return raw;
 bool perspective=p.a<1.5;float4 ink=Print.Load(int3(screen.xy,0));float sceneZ=linear_depth(int2(screen.xy),cal,perspective);
 // Existing ink is lit by the native renderer and may own the native depth pixel.
 if(sceneZ<p.z-.003 || (abs(sceneZ-p.z)<.003&&ink.a<.5))return raw;
 float4 geometry=NormalThickness.Load(int3(screen.xy,0));if(!all(isfinite(geometry))||dot(geometry.xyz,geometry.xyz)<1e-10)return raw;
 float3 incident=perspective?normalize(p.xyz):float3(0,0,1),n=normalize(geometry.xyz);if(dot(n,incident)>0)n=-n;
 float ior=max(IOR,1),thickness=clamp(geometry.a,0,.03);float3 t=refract(incident,n,1/ior);
 float travel=thickness/max(-dot(t,n),.05),original=thickness/max(-dot(incident,n),.05);
 float3 delta=t*travel-incident*original;
 float2 offset=perspective?((p.xy+delta.xy)/max(p.z+delta.z,.01)-p.xy/max(p.z,.01))*cal.zw:delta.xy*cal.zw;
 offset=clamp(offset,-MaximumOffset/size,MaximumOffset/size);float2 sampleUV=clamp(uv+offset,.5/size,1-.5/size);
 if(linear_depth(int2(sampleUV*size),cal,perspective)<=p.z+.003 || Print.SampleLevel(ClampLinear,sampleUV,0).a>.01)sampleUV=uv;
 float3 colour=decode(Scene.SampleLevel(ClampLinear,sampleUV,0).rgb)*exp(-max(Absorption,0)*travel);
 float3 reflection=0;
 // Screen-space reflections only. Missing/off-screen radiance is not invented.
 float3 reflected=reflect(incident,n);float distance=.025;
 [loop]for(uint step=0;step<20;step++){
  float3 q=p.xyz+n*.004+reflected*distance;distance*=1.3;
  if(q.z<=.02)break;
  float2 hitUV=.5+(perspective?q.xy/q.z:q.xy)*cal.zw;
  if(any(hitUV<=0)||any(hitUV>=1))break;
  float z=linear_depth(int2(hitUV*size),cal,perspective);
  if(q.z>=z&&q.z-z<.06&&Print.SampleLevel(ClampLinear,hitUV,0).a<.01){reflection=decode(Scene.SampleLevel(ClampLinear,hitUV,0).rgb);break;}
 }
 float f0=(ior-1)/(ior+1);f0*=f0;float fresnel=f0+(1-f0)*pow(1-saturate(-dot(incident,n)),5);
 float3 pigment=decode(raw.rgb)*exp(-max(Absorption,0)*(clamp(PigmentDepth,0,thickness)/max(-dot(t,n),.05)));
 colour=lerp(colour,pigment,saturate(ink.a));float coat=fresnel*lerp(1,saturate(CoatStrength),saturate(ink.a));
 colour=colour*(1-coat)+reflection*coat*saturate(ReflectionStrength);
 return float4(encode(colour),raw.a);
}
