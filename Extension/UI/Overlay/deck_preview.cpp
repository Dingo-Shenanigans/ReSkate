#include "deck_preview.h"
#include "deck_preview_model.h"
#include "overlay_internal.h"
#include "Engine/Vfs/mod_catalog.h"
#include "Engine/Core/Log/logging.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <vector>

namespace dingosdk::overlay::deck_preview {
namespace {
using namespace model;
constexpr float spin_speed = 0.9f;     // radians a second: a full turn in about seven seconds
constexpr float fade_out_time = 0.16f; // seconds
constexpr float fade_in_time = 0.30f;

// A deck's model and its texture (preview.png, empty for none).
struct DeckModel {
    Mesh mesh;
    detail::CardPixels texture;
};

std::shared_ptr<const DeckModel> stand_in() {
    static const auto mesh = std::make_shared<const DeckModel>(DeckModel{make_stand_in(), {}});
    return mesh;
}

// ---- the decks' models, read off the render thread ------------------------------------------------
struct Entry {
    std::shared_ptr<const DeckModel> mesh;
    std::uint64_t stamp = ~0ull;
};
struct Library {
    std::mutex mutex;
    std::map<std::string, Entry, std::less<>> entries;
    std::string first;      // the deck the carousel is on: read before the others
    bool loading{}, again{};
    ULONGLONG next_look{};
};
Library& library() { static auto* value = new Library; return *value; } // process lifetime, as its thread

std::uint64_t stamp_of(const std::filesystem::path& file) {
    std::error_code error;
    const auto time = std::filesystem::last_write_time(file, error);
    if (error) return 0;
    const auto size = std::filesystem::file_size(file, error);
    return static_cast<std::uint64_t>(time.time_since_epoch().count()) * 1000003ull ^ (error ? 0 : size);
}

std::shared_ptr<const DeckModel> load_deck(const std::string& deck, const std::filesystem::path& folder) {
    const auto obj = folder / L"preview.obj";
    std::error_code error;
    if (folder.empty() || !std::filesystem::is_regular_file(obj, error)) return stand_in();
    std::string problem;
    auto mesh = std::make_shared<DeckModel>();
    if (!model::read(obj, mesh->mesh, problem)) {
        logging::printf(logging::Level::warning, logging::Channel::graphics,
                        "Board Break: the %s deck's preview.obj %s; the preview shows a plain deck.", deck.c_str(), problem.c_str());
        return stand_in();
    }
    const auto png = folder / L"preview.png";
    if (std::filesystem::is_regular_file(png, error)) {
        std::ifstream in(png, std::ios::binary);
        const std::string bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        if (bytes.empty() || bytes.size() > 16 * 1024 * 1024 || !detail::decode_picture(bytes, mesh->texture, 512)) {
            mesh->texture = {};
            logging::printf(logging::Level::warning, logging::Channel::graphics,
                            "Board Break: the %s deck's preview.png is not a PNG of at most 2048 pixels a side; the preview is untextured.",
                            deck.c_str());
        }
    }
    return mesh;
}

void load_decks() {
    auto& l = library();
    for (;;) {
        std::vector<std::string> order;
        {
            std::lock_guard lock(l.mutex);
            l.again = false;
            if (!l.first.empty()) order.push_back(l.first);
        }
        try {
            for (auto& deck : mods::installed_board_break_decks())
                if (std::find(order.begin(), order.end(), deck) == order.end()) order.push_back(std::move(deck));
        } catch (...) {}
        for (const auto& deck : order) {
            std::filesystem::path folder;
            std::uint64_t stamp = 0;
            try {
                folder = mods::board_break_deck_folder(deck);
                if (!folder.empty()) stamp = stamp_of(folder / L"preview.obj") * 31 + stamp_of(folder / L"preview.png") + 1;
            } catch (...) {}
            {
                std::lock_guard lock(l.mutex);
                if (const auto known = l.entries.find(deck); known != l.entries.end() && known->second.stamp == stamp) continue;
            }
            std::shared_ptr<const DeckModel> mesh;
            try { mesh = load_deck(deck, folder); } catch (...) { mesh = stand_in(); }
            std::lock_guard lock(l.mutex);
            l.entries[deck] = {std::move(mesh), stamp};
        }
        std::lock_guard lock(l.mutex);
        if (l.again) continue;
        l.loading = false;
        return;
    }
}

// The model of `deck` once it has been read (a deck not installed is the stand-in); asks for the
// decks to be looked at again every three seconds while the preview is drawn.
std::shared_ptr<const DeckModel> model_of(const std::string& deck) {
    auto& l = library();
    std::lock_guard lock(l.mutex);
    const auto now = GetTickCount64();
    const auto known = l.entries.find(deck);
    const bool missing = known == l.entries.end();
    bool start = missing && l.first != deck; // a deck not read yet: at once
    l.first = deck;
    if (now >= l.next_look) {
        l.next_look = now + 3000;
        start = true;
    }
    if (start) {
        if (l.loading) l.again = true;
        else {
            try {
                std::thread(load_decks).detach();
                l.loading = true;
            } catch (...) {}
        }
    }
    return missing ? nullptr : known->second.mesh;
}


// ---- what the menu shows ---------------------------------------------------------------------------
struct View {
    std::string shown, target;
    std::shared_ptr<const DeckModel> mesh;
    float fade{};   // 0 hidden .. 1 shown
    float spin{};   // the turn, kept across every change of deck
};
View& view() { static View value; return value; }

// What draw() asked of the render thread this frame.
struct Request {
    bool wanted{};
    UINT width{}, height{};
    std::shared_ptr<const DeckModel> mesh;
    float yaw{}, fade{}, time{};
    std::array<float, 3> back{}, glow{};
};
Request& request() { static Request value; return value; }

std::array<float, 3> colour_of(ImU32 c) {
    return {static_cast<float>((c >> IM_COL32_R_SHIFT) & 0xff) / 255.0f, static_cast<float>((c >> IM_COL32_G_SHIFT) & 0xff) / 255.0f,
            static_cast<float>((c >> IM_COL32_B_SHIFT) & 0xff) / 255.0f};
}
float smooth(float t) { t = std::clamp(t, 0.0f, 1.0f); return t * t * (3.0f - 2.0f * t); }
}

// ---- the GPU ---------------------------------------------------------------------------------------
namespace {
using Microsoft::WRL::ComPtr;

// One file of HLSL, built when the preview is first drawn: the backdrop (a soft glow behind the
// deck that falls off to exactly the panel's colour at the window's edges) and the deck itself.
constexpr char shader_source[] = R"hlsl(
cbuffer Preview : register(b0) {
    row_major float4x4 mvp;
    float4 model0, model1, model2; // the deck's turn, by rows
    float4 back;                   // rgb the panel, w the fade
    float4 glow;                   // rgb the light behind the deck, w 1 with a texture
    float4 view;                   // xy the window in pixels, z the camera's distance, w seconds
    float4 extra;                  // x 1 for the stand-in
    float4 unused;
};
Texture2D picture : register(t0);
SamplerState smp : register(s0);

float hash13(float3 p) { p = frac(p * 0.1031); p += dot(p, p.zyx + 31.32); return frac((p.x + p.y) * p.z); }
float noise3(float3 p) {
    float3 i = floor(p), f = frac(p);
    f = f * f * (3.0 - 2.0 * f);
    return lerp(lerp(lerp(hash13(i), hash13(i + float3(1, 0, 0)), f.x), lerp(hash13(i + float3(0, 1, 0)), hash13(i + float3(1, 1, 0)), f.x), f.y),
                lerp(lerp(hash13(i + float3(0, 0, 1)), hash13(i + float3(1, 0, 1)), f.x), lerp(hash13(i + float3(0, 1, 1)), hash13(i + float3(1, 1, 1)), f.x), f.y), f.z);
}
float3 to_linear(float3 c) { return c * c; }
float3 to_display(float3 c) { return sqrt(saturate(c)); }

float3 backdrop(float2 px) {
    float2 c = view.xy * 0.5;
    float2 r = float2(min(view.x * 0.48, view.y * 0.42), view.y * 0.49);
    float d = length((px - c) / max(r, 1.0));
    float amount = pow(1.0 - smoothstep(0.0, 1.0, d), 2.5) * 0.6;
    float dither = (hash13(float3(px, view.w * 60.0)) - 0.5) / 255.0;
    return back.rgb + glow.rgb * amount * 0.55 + dither * step(0.0001, amount);
}

struct BackOut { float4 pos : SV_Position; };
BackOut vs_back(uint id : SV_VertexID) {
    BackOut o;
    float2 p = float2((id << 1) & 2, id & 2);
    o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float4 ps_back(BackOut i) : SV_Target { return float4(backdrop(i.pos.xy), 1); }

struct DeckIn {
    float3 pos : POSITION;
    float3 n : NORMAL;
    float2 uv : TEXCOORD0;
    float2 ps : TEXCOORD1; // ply, side
};
struct DeckOut {
    float4 pos : SV_Position;
    float3 wp : TEXCOORD0;
    float3 n : TEXCOORD1;
    float3 lp : TEXCOORD2;
    float2 uv : TEXCOORD3;
    float ply : TEXCOORD4;
    nointerpolation float side : TEXCOORD5;
};
DeckOut vs_deck(DeckIn i) {
    DeckOut o;
    o.pos = mul(mvp, float4(i.pos, 1));
    o.wp = float3(dot(model0.xyz, i.pos), dot(model1.xyz, i.pos), dot(model2.xyz, i.pos));
    o.n = float3(dot(model0.xyz, i.n), dot(model1.xyz, i.n), dot(model2.xyz, i.n));
    o.lp = i.pos;
    o.uv = i.uv;
    o.ply = i.ps.x;
    o.side = i.ps.y;
    return o;
}

float3 maple(float3 lp) {
    float g = noise3(lp * float3(14, 0.9, 14)) * 1.6 + noise3(lp * float3(70, 4, 70)) * 0.3;
    float rings = abs(frac(lp.x * 8.0 + lp.z * 2.0 + g) - 0.5) * 2.0;
    float3 light = float3(0.87, 0.71, 0.51), dark = float3(0.71, 0.53, 0.35);
    return to_linear(lerp(dark, light, smoothstep(0.1, 0.9, rings)));
}
float3 plies(float ply, float3 lp) {
    float k = saturate(ply) * 7.0;
    float band = min(floor(k), 6.0), f = frac(k);
    float3 c = fmod(band, 2.0) < 1.0 ? float3(0.89, 0.75, 0.56) : float3(0.63, 0.46, 0.31);
    if (band == 3.0) c = float3(0.46, 0.31, 0.19); // the core ply, a shade darker
    c *= lerp(0.6, 1.0, smoothstep(0.0, 0.1, f) * smoothstep(1.0, 0.9, f));
    c *= 0.9 + 0.2 * noise3(lp * float3(45, 3, 45));
    return to_linear(c);
}

float4 ps_deck(DeckOut i) : SV_Target {
    float3 N = normalize(i.n);
    float3 V = normalize(float3(0, 0, view.z) - i.wp);
    if (dot(N, V) < 0) N = -N;
    float3 albedo;
    float spec_power, spec_amount, sparkle = 0;
    float top = i.side > 0.5 ? 1.0 : 0.0;
    if (glow.w > 0.5) {
        albedo = to_linear(picture.Sample(smp, i.uv).rgb);
    } else if (top > 0.5) {
        albedo = float3(0.035, 0.035, 0.038);
    } else if (i.side < -0.5) {
        albedo = maple(i.lp);
    } else {
        albedo = plies(i.ply, i.lp);
    }
    if (top > 0.5) {
        // Grip tape: matt. Without a texture, with a glint off the odd grain of grit; with one, the
        // deck's own art as it is (no grit drawn over it).
        if (glow.w < 0.5) {
            float3 cell = floor(i.lp * 420.0);
            float h = hash13(cell);
            sparkle = step(0.94, h) * hash13(cell + 7.13);
            albedo *= 0.85 + 0.3 * hash13(cell + 3.3);
        }
        spec_power = 20.0;
        spec_amount = 0.05;
    } else {
        spec_power = 90.0; // the clear coat
        spec_amount = 0.35;
    }
    float3 L1 = normalize(float3(0.45, 0.65, 0.85)), C1 = float3(1.0, 0.95, 0.88) * 2.3;
    float3 L2 = normalize(float3(-0.8, -0.15, 0.45)), C2 = float3(0.62, 0.64, 0.7) * 0.6;
    float3 ambient = lerp(float3(0.05, 0.05, 0.06), float3(0.2, 0.21, 0.24), N.y * 0.5 + 0.5);
    float3 colour = albedo * (ambient + C1 * saturate(dot(N, L1)) + C2 * saturate(dot(N, L2)));
    float nv = saturate(dot(N, V));
    float fresnel = 0.04 + 0.96 * pow(1.0 - nv, 5.0);
    float3 H1 = normalize(L1 + V), H2 = normalize(L2 + V);
    colour += C1 * pow(saturate(dot(N, H1)), spec_power) * (spec_amount * (0.3 + fresnel) + sparkle * 1.8);
    colour += C2 * pow(saturate(dot(N, H2)), spec_power) * spec_amount * 0.5;
    // The rim, lit from behind: only the very edge, and not on the grip (matt tape catches none;
    // seen side on, a kick's grip faces away and would turn blue all over).
    float rim = pow(1.0 - nv, 8.0) * (top > 0.5 ? 0.0 : 1.0);
    colour += to_linear(glow.rgb) * rim * 0.4;
    colour = colour / (1.0 + colour) * 1.1;
    float3 shown = to_display(colour);
    if (glow.w > 0.5) {
        // The deck's own art: its colours as painted, lit only gently (never below 70% or much
        // above them, at any turn), with a touch more saturation, the clear coat's highlight and
        // the rim on top. Through the full lighting above, a texture lost a third of its range.
        float3 art = picture.Sample(smp, i.uv).rgb;
        float grey = dot(art, float3(0.299, 0.587, 0.114));
        art = saturate(lerp(float3(grey, grey, grey), art, 1.15));
        float shade = 0.72 + 0.34 * saturate(dot(N, L1)) + 0.08 * saturate(dot(N, L2));
        float3 lit = art * shade;
        lit += to_display(C1 * pow(saturate(dot(N, H1)), spec_power) * spec_amount * (0.3 + fresnel)) * 0.6;
        lit += glow.rgb * rim * 0.25;
        shown = saturate(lit);
    }
    return float4(lerp(backdrop(i.pos.xy), shown, back.w), 1);
}
)hlsl";

struct Constants {
    float mvp[16];
    float model[12];
    float back[4];
    float glow[4];
    float view[4];
    float extra[4];
    float unused[4];
};
static_assert(sizeof(Constants) == 48 * 4);

struct Buffer {
    std::shared_ptr<const DeckModel> mesh;
    ComPtr<ID3D12Resource> resource;
    UINT count{};
};
struct Gpu {
    bool tried{}, failed{};
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> deck, back;
    ComPtr<ID3D12DescriptorHeap> rtv, dsv;
    ComPtr<ID3D12Resource> msaa, resolved, depth;
    UINT width{}, height{}, samples{1};
    D3D12_RESOURCE_STATES msaa_state{}, resolved_state{};
    ImTextureID image{};
    std::vector<Buffer> buffers;
    std::vector<std::pair<UINT64, ComPtr<ID3D12Resource>>> retired; // freed once the GPU is past them
    ComPtr<ID3D12Resource> texture, texture_upload;
    const DeckModel* texture_of{};
    std::shared_ptr<const DeckModel> texture_mesh; // keeps texture_of meaning the same mesh
};
Gpu& gpu() { static Gpu value; return value; }

constexpr UINT preview_slot = 4, texture_slot = 5; // in the overlay's SRV heap (overlay_render.cpp)

D3D12_CPU_DESCRIPTOR_HANDLE cpu_slot(detail::State& s, UINT slot) {
    auto h = s.srvs->GetCPUDescriptorHandleForHeapStart();
    h.ptr += std::size_t{s.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)} * slot;
    return h;
}
D3D12_GPU_DESCRIPTOR_HANDLE gpu_slot(detail::State& s, UINT slot) {
    auto h = s.srvs->GetGPUDescriptorHandleForHeapStart();
    h.ptr += std::uint64_t{s.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)} * slot;
    return h;
}

bool fail(const char* what, HRESULT result) {
    gpu().failed = true;
    logging::printf(logging::Level::warning, logging::Channel::graphics,
                    "Board Break deck preview: %s failed (0x%08lx); the preview is drawn flat instead.", what, static_cast<unsigned long>(result));
    return false;
}

bool make_pipeline(detail::State& s) {
    auto& g = gpu();
    ComPtr<ID3DBlob> vs_deck, ps_deck, vs_back, ps_back, errors;
    const auto compile = [&](const char* entry, const char* target, ComPtr<ID3DBlob>& out) {
        errors.Reset();
        const auto result = D3DCompile(shader_source, sizeof(shader_source) - 1, "deck_preview.hlsl", nullptr, nullptr, entry, target,
                                       D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &out, &errors);
        if (FAILED(result)) {
            logging::printf(logging::Level::warning, logging::Channel::graphics, "Board Break deck preview: shader %s: %s", entry,
                            errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no message");
            return fail("building the shaders", result);
        }
        return true;
    };
    if (!compile("vs_deck", "vs_5_0", vs_deck) || !compile("ps_deck", "ps_5_0", ps_deck) || !compile("vs_back", "vs_5_0", vs_back) ||
        !compile("ps_back", "ps_5_0", ps_back))
        return false;

    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    range.BaseShaderRegister = 0;
    range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants.ShaderRegister = 0;
    parameters[0].Constants.Num32BitValues = sizeof(Constants) / 4;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable.NumDescriptorRanges = 1;
    parameters[1].DescriptorTable.pDescriptorRanges = &range;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_ANISOTROPIC;
    sampler.MaxAnisotropy = 8;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC root{};
    root.NumParameters = 2;
    root.pParameters = parameters;
    root.NumStaticSamplers = 1;
    root.pStaticSamplers = &sampler;
    root.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> signature;
    auto result = D3D12SerializeRootSignature(&root, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &errors);
    if (FAILED(result)) return fail("the root signature", result);
    result = s.device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&g.root));
    if (FAILED(result)) return fail("the root signature", result);

    // 4x MSAA where the card has it for this format (every D3D12 card does), else none.
    g.samples = 1;
    D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS levels{};
    levels.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    levels.SampleCount = 4;
    if (SUCCEEDED(s.device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &levels, sizeof(levels))) && levels.NumQualityLevels)
        g.samples = 4;

    const D3D12_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = g.root.Get();
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; // both sides lit (ps_deck turns the normal to the camera)
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.RasterizerState.MultisampleEnable = g.samples > 1;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pso.SampleDesc.Count = g.samples;
    pso.VS = {vs_back->GetBufferPointer(), vs_back->GetBufferSize()};
    pso.PS = {ps_back->GetBufferPointer(), ps_back->GetBufferSize()};
    result = s.device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&g.back));
    if (FAILED(result)) return fail("the backdrop's pipeline", result);
    pso.InputLayout = {layout, static_cast<UINT>(std::size(layout))};
    pso.DepthStencilState.DepthEnable = TRUE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    pso.VS = {vs_deck->GetBufferPointer(), vs_deck->GetBufferSize()};
    pso.PS = {ps_deck->GetBufferPointer(), ps_deck->GetBufferSize()};
    result = s.device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&g.deck));
    if (FAILED(result)) return fail("the deck's pipeline", result);

    D3D12_DESCRIPTOR_HEAP_DESC heap{};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heap.NumDescriptors = 1;
    if (FAILED(result = s.device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&g.rtv)))) return fail("the render target's heap", result);
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    if (FAILED(result = s.device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&g.dsv)))) return fail("the depth buffer's heap", result);
    return true;
}

void retire(ComPtr<ID3D12Resource>& resource) {
    if (resource) gpu().retired.emplace_back(detail::state().next_fence + 1, std::move(resource));
    resource.Reset();
}

// The window's textures, at least `width` x `height` (grown in steps, so a menu dragged larger
// does not make new ones every frame).
bool make_targets(detail::State& s, UINT width, UINT height) {
    auto& g = gpu();
    if (g.resolved && width <= g.width && height <= g.height) return true;
    const UINT w = (std::max(width, g.width) + 63) / 64 * 64, h = (std::max(height, g.height) + 63) / 64 * 64;
    if (w > 4096 || h > 4096) return false;
    // ImGui's draws in frames still on the GPU sample the old one through the same slot.
    if (g.resolved && !detail::completed(s.next_fence, detail::render_fence_timeout_ms)) return false;
    retire(g.msaa), retire(g.resolved), retire(g.depth);
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = w;
    desc.Height = h;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE clear{};
    clear.Format = desc.Format;
    HRESULT result{};
    const auto render_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    if (FAILED(result = s.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, render_state, &clear, IID_PPV_ARGS(&g.resolved))))
        return fail("the preview's texture", result);
    g.resolved_state = render_state;
    if (g.samples > 1) {
        desc.SampleDesc.Count = g.samples;
        if (FAILED(result = s.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RESOLVE_SOURCE, &clear,
                                                              IID_PPV_ARGS(&g.msaa))))
            return fail("the preview's MSAA target", result);
        g.msaa_state = D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
    }
    desc.Format = DXGI_FORMAT_D32_FLOAT;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
    clear.Format = desc.Format;
    clear.DepthStencil.Depth = 1.0f;
    if (FAILED(result = s.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear,
                                                          IID_PPV_ARGS(&g.depth))))
        return fail("the preview's depth buffer", result);
    s.device->CreateRenderTargetView(g.samples > 1 ? g.msaa.Get() : g.resolved.Get(), nullptr, g.rtv->GetCPUDescriptorHandleForHeapStart());
    s.device->CreateDepthStencilView(g.depth.Get(), nullptr, g.dsv->GetCPUDescriptorHandleForHeapStart());
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Texture2D.MipLevels = 1;
    s.device->CreateShaderResourceView(g.resolved.Get(), &view, cpu_slot(s, preview_slot));
    g.image = static_cast<ImTextureID>(gpu_slot(s, preview_slot).ptr);
    g.width = w, g.height = h;
    return true;
}

// The mesh's vertices on the GPU (an upload heap buffer: a preview's few thousand triangles, made once).
const Buffer* buffer_for(detail::State& s, const std::shared_ptr<const DeckModel>& mesh) {
    auto& g = gpu();
    for (auto& b : g.buffers)
        if (b.mesh == mesh) return &b;
    // Models no deck holds any more (a re-exported preview.obj), once the GPU is done with them.
    for (auto it = g.buffers.begin(); it != g.buffers.end();)
        if (it->mesh.use_count() == 1 && it->mesh != stand_in()) { retire(it->resource); it = g.buffers.erase(it); }
        else ++it;
    const UINT64 bytes = mesh->mesh.vertices.size() * sizeof(Vertex);
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Buffer made;
    if (FAILED(s.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                 IID_PPV_ARGS(&made.resource))))
        return nullptr;
    void* mapped{};
    const D3D12_RANGE none{};
    if (FAILED(made.resource->Map(0, &none, &mapped)) || !mapped) return nullptr;
    std::memcpy(mapped, mesh->mesh.vertices.data(), bytes);
    made.resource->Unmap(0, nullptr);
    made.mesh = mesh;
    made.count = static_cast<UINT>(mesh->mesh.vertices.size());
    g.buffers.push_back(std::move(made));
    return &g.buffers.back();
}

void barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES& now, D3D12_RESOURCE_STATES next) {
    if (now == next) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = resource;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = now;
    b.Transition.StateAfter = next;
    list->ResourceBarrier(1, &b);
    now = next;
}

// ---- without the shaders: ImGui's own triangles, painted back to front -----------------------------
void draw_flat(ImDrawList* draw, ImVec2 a, ImVec2 z, const Mesh& mesh, float yaw, float fade, ImU32 background) {
    const float w = z.x - a.x, h = z.y - a.y;
    const auto cam = camera(yaw, w / h);
    const auto full = cam.view_projection * cam.model;
    const auto back = colour_of(background);
    struct Flat { ImVec2 p[3]; float depth; ImU32 colour; };
    std::vector<Flat> flats;
    flats.reserve(mesh.vertices.size() / 3);
    constexpr float light[3] = {0.39f, 0.56f, 0.73f}; // (0.45, 0.65, 0.85), normalised: the shaders' key light
    for (std::size_t i = 0; i + 2 < mesh.vertices.size(); i += 3) {
        Flat f{};
        float depth = 0, n[3]{};
        for (int k = 0; k < 3; ++k) {
            const auto& v = mesh.vertices[i + static_cast<std::size_t>(k)];
            float clip[4]{};
            for (int r = 0; r < 4; ++r)
                clip[r] = full.m[r][0] * v.position[0] + full.m[r][1] * v.position[1] + full.m[r][2] * v.position[2] + full.m[r][3];
            if (clip[3] <= 1e-4f) { depth = -1; break; }
            f.p[k] = ImVec2(a.x + (clip[0] / clip[3] * 0.5f + 0.5f) * w, a.y + (0.5f - clip[1] / clip[3] * 0.5f) * h);
            depth += clip[3];
            for (int j = 0; j < 3; ++j) n[j] += v.normal[j];
        }
        if (depth < 0) continue;
        f.depth = depth;
        float wn[3]{}, l = 0;
        for (int r = 0; r < 3; ++r) {
            wn[r] = cam.model.m[r][0] * n[0] + cam.model.m[r][1] * n[1] + cam.model.m[r][2] * n[2];
            l += wn[r] * wn[r];
        }
        l = l > 1e-12f ? 1.0f / std::sqrt(l) : 0.0f;
        const float facing = std::abs((wn[0] * light[0] + wn[1] * light[1] + wn[2] * light[2]) * l);
        const float side = mesh.vertices[i].side;
        const std::array<float, 3> base = side > 0.5f ? std::array<float, 3>{0.12f, 0.12f, 0.13f}
                                         : side < -0.5f ? std::array<float, 3>{0.85f, 0.68f, 0.48f}
                                                        : std::array<float, 3>{0.72f, 0.56f, 0.38f};
        const float lit = 0.35f + 0.75f * facing;
        const int shifts[3] = {IM_COL32_R_SHIFT, IM_COL32_G_SHIFT, IM_COL32_B_SHIFT};
        ImU32 c = IM_COL32(0, 0, 0, 255);
        for (std::size_t k = 0; k < 3; ++k) {
            const float value = back[k] + (std::min(1.0f, base[k] * lit) - back[k]) * fade;
            c |= static_cast<ImU32>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f) << shifts[k];
        }
        f.colour = c;
        flats.push_back(f);
    }
    std::sort(flats.begin(), flats.end(), [](const Flat& x, const Flat& y) { return x.depth > y.depth; });
    const auto flags = draw->Flags;
    draw->Flags &= ~ImDrawListFlags_AntiAliasedFill; // no seams between the triangles
    for (const auto& f : flats) draw->AddTriangleFilled(f.p[0], f.p[1], f.p[2], f.colour);
    draw->Flags = flags;
}
}

void draw(ImDrawList* draw, ImVec2 a, ImVec2 z, const std::string& deck, ImU32 background) {
    a = ImVec2(std::floor(a.x), std::floor(a.y));
    z = ImVec2(std::floor(z.x), std::floor(z.y));
    const float w = z.x - a.x, h = z.y - a.y;
    draw->AddRectFilled(a, z, background);
    if (w < 16 || h < 16) return;
    auto& v = view();
    const float dt = std::clamp(ImGui::GetIO().DeltaTime, 0.0f, 0.1f);
    v.spin = std::fmod(v.spin + dt * spin_speed, 2.0f * pi);
    v.target = deck;
    const auto wanted = deck.empty() ? nullptr : model_of(deck);
    if (v.shown != v.target || (wanted && wanted != v.mesh)) {
        // Out first; the model is changed only while it cannot be seen.
        v.fade = std::max(0.0f, v.fade - dt / fade_out_time);
        if (v.fade <= 0.0f && (wanted || v.target.empty())) {
            v.shown = v.target;
            v.mesh = wanted;
        }
    } else if (v.mesh) {
        v.fade = std::min(1.0f, v.fade + dt / fade_in_time);
    }
    const float fade = smooth(v.fade);
    auto& g = gpu();
    if (!g.failed) {
        auto& r = request();
        r.wanted = true;
        r.width = static_cast<UINT>(w), r.height = static_cast<UINT>(h);
        r.mesh = v.mesh;
        r.yaw = v.spin;
        r.fade = fade;
        r.time = static_cast<float>(ImGui::GetTime());
        r.back = colour_of(background);
        r.glow = colour_of(IM_COL32(1, 131, 255, 255)); // ReSkate blue (skate_theme::blue)
        // Drawn by render_deck_preview just before ImGui's draws; until its texture is big enough
        // (the first frame, a larger menu) the panel shows alone.
        if (g.image && g.width >= r.width && g.height >= r.height)
            draw->AddImage(g.image, a, ImVec2(a.x + w, a.y + h), ImVec2(0, 0),
                           ImVec2(w / static_cast<float>(g.width), h / static_cast<float>(g.height)));
    }
    if (g.failed && v.mesh && fade > 0.0f) {
        draw->PushClipRect(a, z, true);
        draw_flat(draw, a, z, v.mesh->mesh, v.spin, fade, background);
        draw->PopClipRect();
    }
}

bool showing_stand_in() {
    const auto& v = view();
    return v.mesh && v.mesh->mesh.stand_in;
}
}

namespace dingosdk::overlay::detail {
void render_deck_preview(State& s) {
    using namespace deck_preview;
    auto& r = request();
    if (!r.wanted) return;
    r.wanted = false;
    auto& g = gpu();
    if (g.failed || !s.device || !s.srvs || !s.commands) return;
    // Resources this renderer let go of, once no frame on the GPU uses them.
    std::erase_if(g.retired, [](const auto& entry) { return completed(entry.first, 0); });
    if (!g.tried) {
        g.tried = true;
        if (!make_pipeline(s)) return;
    }
    if (!g.root || !make_targets(s, r.width, r.height)) return;
    auto* list = s.commands.Get();

    // The model's texture in the heap's sixth slot, while the GPU no longer samples the last one.
    bool textured = false;
    if (r.mesh && !r.mesh->texture.rgba.empty()) {
        if (g.texture_of != r.mesh.get() && (!g.texture || completed(s.next_fence, render_fence_timeout_ms))) {
            if (upload_image(s, r.mesh->texture, texture_slot, g.texture, g.texture_upload)) {
                g.texture_of = r.mesh.get();
                g.texture_mesh = r.mesh;
            } else {
                g.texture_of = nullptr;
                g.texture_mesh.reset();
            }
        }
        textured = g.texture_of == r.mesh.get();
    }
    const Buffer* vertices = r.mesh && r.fade > 0.0f ? buffer_for(s, r.mesh) : nullptr;

    ID3D12Resource* target = g.samples > 1 ? g.msaa.Get() : g.resolved.Get();
    auto& target_state = g.samples > 1 ? g.msaa_state : g.resolved_state;
    barrier(list, target, target_state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const auto rtv = g.rtv->GetCPUDescriptorHandleForHeapStart();
    const auto dsv = g.dsv->GetCPUDescriptorHandleForHeapStart();
    list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    const float clear[4] = {r.back[0], r.back[1], r.back[2], 1.0f};
    const D3D12_RECT area{0, 0, static_cast<LONG>(r.width), static_cast<LONG>(r.height)};
    list->ClearRenderTargetView(rtv, clear, 1, &area);
    list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 1, &area);
    D3D12_VIEWPORT viewport{0, 0, static_cast<float>(r.width), static_cast<float>(r.height), 0, 1};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &area);
    ID3D12DescriptorHeap* heaps[] = {s.srvs.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetGraphicsRootSignature(g.root.Get());
    list->SetGraphicsRootDescriptorTable(1, gpu_slot(s, textured ? texture_slot : 0));

    const auto cam = camera(r.yaw, static_cast<float>(r.width) / static_cast<float>(r.height));
    const auto mvp = cam.view_projection * cam.model;
    Constants c{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) c.mvp[i * 4 + j] = mvp.m[i][j];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j) c.model[i * 4 + j] = j < 3 ? cam.model.m[i][j] : 0.0f;
    c.back[0] = r.back[0], c.back[1] = r.back[1], c.back[2] = r.back[2], c.back[3] = r.fade;
    c.glow[0] = r.glow[0], c.glow[1] = r.glow[1], c.glow[2] = r.glow[2], c.glow[3] = textured ? 1.0f : 0.0f;
    c.view[0] = static_cast<float>(r.width), c.view[1] = static_cast<float>(r.height), c.view[2] = cam.distance, c.view[3] = r.time;
    c.extra[0] = r.mesh && r.mesh->mesh.stand_in ? 1.0f : 0.0f;
    list->SetGraphicsRoot32BitConstants(0, sizeof(Constants) / 4, &c, 0);

    list->SetPipelineState(g.back.Get());
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->DrawInstanced(3, 1, 0, 0);
    if (vertices) {
        D3D12_VERTEX_BUFFER_VIEW view{};
        view.BufferLocation = vertices->resource->GetGPUVirtualAddress();
        view.SizeInBytes = vertices->count * static_cast<UINT>(sizeof(Vertex));
        view.StrideInBytes = sizeof(Vertex);
        list->SetPipelineState(g.deck.Get());
        list->IASetVertexBuffers(0, 1, &view);
        list->DrawInstanced(vertices->count, 1, 0, 0);
    }
    if (g.samples > 1) {
        barrier(list, g.msaa.Get(), g.msaa_state, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
        barrier(list, g.resolved.Get(), g.resolved_state, D3D12_RESOURCE_STATE_RESOLVE_DEST);
        list->ResolveSubresource(g.resolved.Get(), 0, g.msaa.Get(), 0, DXGI_FORMAT_R8G8B8A8_UNORM);
    }
    barrier(list, g.resolved.Get(), g.resolved_state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

void deck_preview_lost() {
    using namespace deck_preview;
    auto& g = gpu();
    g = Gpu{}; // with the device: built again on the next device, from scratch
    request() = {};
}
}
