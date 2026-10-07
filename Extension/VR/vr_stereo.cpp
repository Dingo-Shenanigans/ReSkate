#include "vr_stereo.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <cstring>
#include <format>

namespace dingosdk::vr::detail {
using Microsoft::WRL::ComPtr;
namespace {
// Full-screen triangle. For each output pixel of one eye, search the source row for
// the point that the eye's parallax moves here: x + side * shift(x) = u, where
// shift grows with the reversed-Z depth (near = large). The nearest match wins;
// a pixel nothing lands on (a hole behind a near edge) takes the background.
constexpr char shader_source[] = R"(
Texture2D color : register(t0);
Texture2D<float> depth : register(t1);
SamplerState linear_clamp : register(s0);
cbuffer Constants : register(b0) { uint mode; float side; float scale; float max_shift; float vignette; };
struct Vertex { float4 position : SV_Position; float2 uv : TEXCOORD0; };
Vertex vs_main(uint id : SV_VertexID) {
    Vertex v;
    float2 t = float2((id << 1) & 2, id & 2);
    v.position = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    v.uv = t;
    return v;
}
float3 srgb_to_linear(float3 c) {
    float3 low = c / 12.92;
    float3 high = pow((c + 0.055) / 1.055, 2.4);
    return lerp(high, low, step(c, 0.04045));
}
float shift_at(float2 uv, float2 size) {
    int2 p = int2(clamp(uv, 0, 0.99999) * size);
    return depth.Load(int3(p, 0)) * scale;
}
// Marches from the largest shift down: the first source point whose own shift
// reaches the ray (f >= 0) is the nearest surface along it. The crossing is then
// interpolated between the two steps for a sub-pixel, stable edge. At t = 0 the
// test always passes. A jump from clearly behind to clearly in front is a depth
// edge, not a surface: the pixel is a hole the centre view never saw. Stretch
// fills it with the near edge; background fill (mode bit 8) with the farthest
// surface the march passed.
// Widens the near depth by a few pixels (max filter, reversed Z: larger = nearer), once
// per kept frame: anti-aliased and fringed edge pixels then move with their object
// instead of tearing off as lines between the eyes.
// `mode` holds the radius in pixels here (0-3); 0 copies the depth as it is.
float ps_dilate(Vertex v) : SV_Target {
    float2 size;
    depth.GetDimensions(size.x, size.y);
    int2 p = int2(v.position.xy);
    int radius = int(mode);
    int rows = min(radius, 1);
    float nearest = 0;
    [loop] for (int dy = -rows; dy <= rows; ++dy)
        [loop] for (int dx = -radius; dx <= radius; ++dx)
            nearest = max(nearest, depth.Load(int3(clamp(p + int2(dx, dy), int2(0, 0), int2(size) - 1), 0)));
    return nearest;
}
static const int steps = 48;
// Comfort vignette: darkens towards the edges by `amount` (0 none, 1 a narrow window). The
// image is rendered wider than the lenses show, so it starts well inside the image edge.
float3 comfort(float3 c, float2 uv, float amount) {
    if (amount <= 0) return c;
    float r = length((uv - 0.5) * 2);
    float inner = lerp(0.95, 0.15, amount);
    return c * (1 - smoothstep(inner, inner + 0.35, r));
}
float4 ps_main(Vertex v) : SV_Target {
    float x = v.uv.x;
    uint decode = mode & 0xff;
    bool background_fill = (mode & 0x100) != 0;
    if (scale > 0 && max_shift > 0) {
        float2 size;
        depth.GetDimensions(size.x, size.y);
        float step_size = max_shift / steps;
        float previous_t = max_shift, previous_f = shift_at(float2(v.uv.x - side * max_shift, v.uv.y), size) - max_shift;
        float far_shift = previous_f + max_shift, far_x = v.uv.x - side * max_shift;
        [loop] for (int i = steps - 1; i >= 0; --i) {
            float t = step_size * i;
            float sx = v.uv.x - side * t;
            float s = shift_at(float2(sx, v.uv.y), size);
            float f = s - t;
            if (f >= 0) {
                bool hole = previous_f < -2 * step_size && f > 2 * step_size;
                float crossing = previous_f < 0 ? t + (previous_t - t) * f / max(f - previous_f, 1e-6) : t;
                x = hole && background_fill ? far_x : v.uv.x - side * crossing;
                break;
            }
            if (s < far_shift) { far_shift = s; far_x = sx; }
            previous_t = t;
            previous_f = f;
        }
    }
    float3 c = saturate(color.SampleLevel(linear_clamp, float2(x, v.uv.y), 0).rgb);
    if (decode == 0) c = srgb_to_linear(c);
    return float4(comfort(c, v.uv, vignette), 1);
}
)";

template<class Function>
Function system_function(const wchar_t* module_name, const char* name) {
    HMODULE module = GetModuleHandleW(module_name);
    if (!module) module = LoadLibraryExW(module_name, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return module ? reinterpret_cast<Function>(reinterpret_cast<void*>(GetProcAddress(module, name))) : nullptr;
}
std::string hresult(const char* what, HRESULT result) {
    return std::format("{} failed (0x{:08x}).", what, static_cast<unsigned long>(result));
}
bool compile(pD3DCompile compiler, const char* entry, const char* target, ComPtr<ID3DBlob>& out, std::string& error) {
    ComPtr<ID3DBlob> messages;
    const HRESULT result = compiler(shader_source, sizeof(shader_source) - 1, "reskate_vr_stereo", nullptr, nullptr, entry,
        target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &out, &messages);
    if (SUCCEEDED(result)) return true;
    error = hresult("Compiling the VR stereo shader", result);
    if (messages) error += std::string(" ") + static_cast<const char*>(messages->GetBufferPointer());
    return false;
}
D3D12_RESOURCE_BARRIER transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after,
    UINT subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {resource, subresource, before, after};
    return barrier;
}
}

bool DepthStereo::create(ID3D12Device* device, std::string& error) {
    destroy();
    const auto compiler = system_function<pD3DCompile>(L"d3dcompiler_47.dll", "D3DCompile");
    const auto serialize = system_function<PFN_D3D12_SERIALIZE_ROOT_SIGNATURE>(L"d3d12.dll", "D3D12SerializeRootSignature");
    if (!compiler || !serialize) { error = "d3dcompiler_47.dll or d3d12.dll is unavailable."; return false; }
    if (!compile(compiler, "vs_main", "vs_5_0", vertex_, error) || !compile(compiler, "ps_main", "ps_5_0", pixel_, error) ||
        !compile(compiler, "ps_dilate", "ps_5_0", dilate_, error))
        return false;
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 2;
    std::array<D3D12_ROOT_PARAMETER, 2> parameters{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].DescriptorTable = {1, &range};
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[1].Constants = {0, 0, 5};
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC root{};
    root.NumParameters = static_cast<UINT>(parameters.size());
    root.pParameters = parameters.data();
    root.NumStaticSamplers = 1;
    root.pStaticSamplers = &sampler;
    ComPtr<ID3DBlob> blob, messages;
    HRESULT result = serialize(&root, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &messages);
    if (FAILED(result)) { error = hresult("Serializing the VR stereo root signature", result); return false; }
    result = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root_));
    if (FAILED(result)) { error = hresult("Creating the VR stereo root signature", result); return false; }
    D3D12_DESCRIPTOR_HEAP_DESC heap{};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors = slots * 4; // per slot: colour + depth for the eyes, (unused) + raw depth for the widening
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    result = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&srvs_));
    if (FAILED(result)) { error = hresult("Creating the VR stereo SRV heap", result); return false; }
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    result = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&rtvs_));
    if (FAILED(result)) { error = hresult("Creating the VR stereo RTV heap", result); return false; }
    srv_stride_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    rtv_stride_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for (auto& allocator : allocators_) {
        result = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
        if (FAILED(result)) { error = hresult("Creating a VR stereo command allocator", result); return false; }
    }
    result = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators_[0].Get(), nullptr, IID_PPV_ARGS(&list_));
    if (FAILED(result)) { error = hresult("Creating the VR stereo command list", result); return false; }
    list_->Close();
    list_->SetName(L"ReSkate VR depth stereo");
    result = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_));
    if (FAILED(result)) { error = hresult("Creating the VR stereo fence", result); return false; }
    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event_) { error = "Creating the VR stereo fence event failed."; return false; }
    device_ = device;
    return true;
}

bool DepthStereo::wait(UINT64 value, DWORD timeout_ms) noexcept {
    if (!value || !fence_) return true;
    const auto done = fence_->GetCompletedValue();
    if (done == UINT64_MAX) return false;
    if (done >= value) return true;
    if (FAILED(fence_->SetEventOnCompletion(value, event_))) return false;
    if (WaitForSingleObject(event_, timeout_ms) != WAIT_OBJECT_0) return false;
    return fence_->GetCompletedValue() >= value;
}
bool DepthStereo::wait_idle(DWORD timeout_ms) noexcept { return wait(next_fence_, timeout_ms); }

void DepthStereo::destroy() noexcept {
    if (fence_) (void)wait_idle(2000);
    pipelines_.clear();
    list_.Reset();
    for (auto& allocator : allocators_) allocator.Reset();
    slot_fences_ = {};
    srvs_.Reset(); rtvs_.Reset(); root_.Reset(); vertex_.Reset(); pixel_.Reset(); dilate_.Reset(); fence_.Reset(); kept_.Reset();
    kept_depth_.Reset(); widened_.Reset();
    if (event_) CloseHandle(event_);
    event_ = nullptr;
    next_fence_ = 0;
    slot_ = 0;
    kept_format_ = DXGI_FORMAT_UNKNOWN;
    device_.Reset();
}

ID3D12PipelineState* DepthStereo::pipeline(DXGI_FORMAT format, std::string& error, bool dilate) {
    const int key = static_cast<int>(format) | (dilate ? 0x10000 : 0);
    if (const auto found = pipelines_.find(key); found != pipelines_.end()) return found->second.Get();
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = root_.Get();
    desc.VS = {vertex_->GetBufferPointer(), vertex_->GetBufferSize()};
    const auto& shader = dilate ? dilate_ : pixel_;
    desc.PS = {shader->GetBufferPointer(), shader->GetBufferSize()};
    for (auto& target : desc.BlendState.RenderTarget) {
        target.SrcBlend = target.SrcBlendAlpha = D3D12_BLEND_ONE;
        target.DestBlend = target.DestBlendAlpha = D3D12_BLEND_ZERO;
        target.BlendOp = target.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        target.LogicOp = D3D12_LOGIC_OP_NOOP;
        target.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }
    desc.DepthStencilState.DepthEnable = FALSE;
    desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    desc.DepthStencilState.FrontFace = desc.DepthStencilState.BackFace = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
        D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS};
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = format;
    desc.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> state;
    const HRESULT result = device_->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&state));
    if (FAILED(result)) { error = hresult("Creating the VR stereo pipeline", result); return nullptr; }
    return (pipelines_[key] = state).Get();
}

bool DepthStereo::begin(std::string& error) {
    if (!device_) { error = "The VR stereo pass is not initialised."; return false; }
    if (!wait(slot_fences_[slot_], 1000)) { error = "The GPU did not finish an earlier VR stereo pass."; return false; }
    HRESULT result = allocators_[slot_]->Reset();
    if (SUCCEEDED(result)) result = list_->Reset(allocators_[slot_].Get(), nullptr);
    if (FAILED(result)) { error = hresult("Resetting the VR stereo command list", result); return false; }
    return true;
}

bool DepthStereo::submit(ID3D12CommandQueue* queue, std::string& error) {
    HRESULT result = list_->Close();
    if (FAILED(result)) { error = hresult("Closing the VR stereo command list", result); return false; }
    ID3D12CommandList* lists[] = {list_.Get()};
    queue->ExecuteCommandLists(1, lists);
    const UINT64 signal = ++next_fence_;
    result = queue->Signal(fence_.Get(), signal);
    if (FAILED(result)) { error = hresult("Signalling the VR stereo fence", result); return false; }
    slot_fences_[slot_] = signal;
    slot_ = (slot_ + 1) % slots;
    return true;
}

bool DepthStereo::keep(ID3D12CommandQueue* queue, ID3D12Resource* frame, std::string& error) {
    const auto desc = frame->GetDesc();
    if (!kept_ || kept_->GetDesc().Width != desc.Width || kept_->GetDesc().Height != desc.Height || kept_format_ != desc.Format) {
        if (!wait_idle(2000)) { error = "The GPU did not finish an earlier VR stereo pass."; return false; }
        kept_.Reset();
        auto copy = desc;
        copy.Flags = D3D12_RESOURCE_FLAG_NONE;
        copy.MipLevels = 1;
        D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
        const HRESULT result = device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &copy,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&kept_));
        if (FAILED(result)) { error = hresult("Creating the VR stereo colour copy", result); return false; }
        kept_format_ = desc.Format;
    }
    if (!begin(error)) return false;
    const std::array<D3D12_RESOURCE_BARRIER, 2> before{
        transition(frame, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE),
        transition(kept_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST)};
    list_->ResourceBarrier(static_cast<UINT>(before.size()), before.data());
    list_->CopyResource(kept_.Get(), frame);
    const std::array<D3D12_RESOURCE_BARRIER, 2> after{
        transition(frame, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT),
        transition(kept_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)};
    list_->ResourceBarrier(static_cast<UINT>(after.size()), after.data());
    return submit(queue, error);
}

bool DepthStereo::keep_depth(ID3D12CommandQueue* queue, ID3D12Resource* depth, int widening, std::string& error) {
    const auto desc = depth->GetDesc();
    if (!kept_depth_ || kept_depth_->GetDesc().Width != desc.Width || kept_depth_->GetDesc().Height != desc.Height ||
        kept_depth_->GetDesc().Format != desc.Format) {
        if (!wait_idle(2000)) { error = "The GPU did not finish an earlier VR stereo pass."; return false; }
        kept_depth_.Reset();
        D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
        const HRESULT result = device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&kept_depth_));
        if (FAILED(result)) { error = hresult("Creating the VR stereo depth copy", result); return false; }
    }
    if (!begin(error)) return false;
    const std::array<D3D12_RESOURCE_BARRIER, 2> before{
        transition(depth, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE, 0),
        transition(kept_depth_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST, 0)};
    list_->ResourceBarrier(static_cast<UINT>(before.size()), before.data());
    D3D12_TEXTURE_COPY_LOCATION to{kept_depth_.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
    D3D12_TEXTURE_COPY_LOCATION from{depth, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
    list_->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    const std::array<D3D12_RESOURCE_BARRIER, 2> after{
        transition(depth, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST, 0),
        transition(kept_depth_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, 0)};
    list_->ResourceBarrier(static_cast<UINT>(after.size()), after.data());

    // Widen the near depth into an R32 target the eyes read.
    if (!widened_ || widened_->GetDesc().Width != desc.Width || widened_->GetDesc().Height != desc.Height) {
        widened_.Reset();
        D3D12_RESOURCE_DESC target{};
        target.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        target.Width = desc.Width;
        target.Height = desc.Height;
        target.DepthOrArraySize = target.MipLevels = 1;
        target.Format = DXGI_FORMAT_R32_FLOAT;
        target.SampleDesc.Count = 1;
        target.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
        const HRESULT result = device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &target,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&widened_));
        if (FAILED(result)) { error = hresult("Creating the VR stereo widened depth", result); return false; }
    }
    auto* state = pipeline(DXGI_FORMAT_R32_FLOAT, error, true);
    if (!state) return false;
    list_->SetPipelineState(state);
    auto srv = srvs_->GetCPUDescriptorHandleForHeapStart();
    srv.ptr += static_cast<SIZE_T>(slot_ * 4 + 2) * srv_stride_;
    auto srv_gpu = srvs_->GetGPUDescriptorHandleForHeapStart();
    srv_gpu.ptr += static_cast<UINT64>(slot_ * 4 + 2) * srv_stride_;
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Texture2D.MipLevels = 1;
    view.Format = kept_format_ != DXGI_FORMAT_UNKNOWN ? kept_format_ : DXGI_FORMAT_R8G8B8A8_UNORM;
    if (kept_) device_->CreateShaderResourceView(kept_.Get(), &view, srv); // t0, unused by the widening
    view.Format = DXGI_FORMAT_R32_FLOAT; // the depth copy is R32_TYPELESS (vr_depth.cpp)
    auto depth_srv = srv;
    depth_srv.ptr += srv_stride_;
    device_->CreateShaderResourceView(kept_depth_.Get(), &view, depth_srv);
    auto rtv = rtvs_->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(slot_ * 2) * rtv_stride_;
    D3D12_RENDER_TARGET_VIEW_DESC target_view{};
    target_view.Format = DXGI_FORMAT_R32_FLOAT;
    target_view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    device_->CreateRenderTargetView(widened_.Get(), &target_view, rtv);
    list_->ResourceBarrier(1, std::array{transition(widened_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_RENDER_TARGET)}.data());
    ID3D12DescriptorHeap* heaps[] = {srvs_.Get()};
    list_->SetDescriptorHeaps(1, heaps);
    list_->SetGraphicsRootSignature(root_.Get());
    list_->SetGraphicsRootDescriptorTable(0, srv_gpu);
    const std::array<UINT, 4> radius{static_cast<UINT>(std::clamp(widening, 0, 3)), 0, 0, 0};
    list_->SetGraphicsRoot32BitConstants(1, static_cast<UINT>(radius.size()), radius.data(), 0);
    list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const D3D12_VIEWPORT viewport{0, 0, static_cast<float>(desc.Width), static_cast<float>(desc.Height), 0, 1};
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(desc.Width), static_cast<LONG>(desc.Height)};
    list_->RSSetViewports(1, &viewport);
    list_->RSSetScissorRects(1, &scissor);
    list_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    list_->DrawInstanced(3, 1, 0, 0);
    list_->ResourceBarrier(1, std::array{transition(widened_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)}.data());
    return submit(queue, error);
}

bool DepthStereo::render(ID3D12CommandQueue* queue, const std::array<ID3D12Resource*, 2>& targets, DXGI_FORMAT target_format,
    const StereoParams& params, bool raw, std::string& error) {
    if (!kept_ || !widened_) { error = "No frame kept for the VR stereo pass yet."; return false; }
    auto* state = pipeline(target_format, error);
    if (!state || !begin(error)) return false;
    list_->SetPipelineState(state);
    auto srv = srvs_->GetCPUDescriptorHandleForHeapStart();
    srv.ptr += static_cast<SIZE_T>(slot_) * 4 * srv_stride_;
    auto srv_gpu = srvs_->GetGPUDescriptorHandleForHeapStart();
    srv_gpu.ptr += static_cast<UINT64>(slot_) * 4 * srv_stride_;
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = kept_format_;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Texture2D.MipLevels = 1;
    device_->CreateShaderResourceView(kept_.Get(), &view, srv);
    view.Format = DXGI_FORMAT_R32_FLOAT;
    auto depth_srv = srv;
    depth_srv.ptr += srv_stride_;
    device_->CreateShaderResourceView(widened_.Get(), &view, depth_srv);

    ID3D12DescriptorHeap* heaps[] = {srvs_.Get()};
    list_->SetDescriptorHeaps(1, heaps);
    list_->SetGraphicsRootSignature(root_.Get());
    list_->SetGraphicsRootDescriptorTable(0, srv_gpu);
    list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const UINT mode = (raw ? 2u : kept_format_ == DXGI_FORMAT_R16G16B16A16_FLOAT ? 1u : 0u) | (params.background_fill ? 0x100u : 0u);
    for (std::size_t eye = 0; eye < targets.size(); ++eye) {
        auto rtv = rtvs_->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += static_cast<SIZE_T>(slot_ * 2 + eye) * rtv_stride_;
        D3D12_RENDER_TARGET_VIEW_DESC target_view{};
        target_view.Format = target_format;
        target_view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        device_->CreateRenderTargetView(targets[eye], &target_view, rtv);
        // Left eye (+1): near points move right in its image; right eye (-1): left.
        const float side = eye == 0 ? 1.0f : -1.0f;
        std::array<UINT, 5> constants{mode, 0, 0, 0, 0};
        std::memcpy(&constants[1], &side, sizeof(float));
        std::memcpy(&constants[2], &params.scale, sizeof(float));
        std::memcpy(&constants[3], &params.max_shift, sizeof(float));
        std::memcpy(&constants[4], &params.vignette, sizeof(float));
        list_->SetGraphicsRoot32BitConstants(1, static_cast<UINT>(constants.size()), constants.data(), 0);
        const auto desc = targets[eye]->GetDesc();
        const D3D12_VIEWPORT viewport{0, 0, static_cast<float>(desc.Width), static_cast<float>(desc.Height), 0, 1};
        const D3D12_RECT scissor{0, 0, static_cast<LONG>(desc.Width), static_cast<LONG>(desc.Height)};
        list_->RSSetViewports(1, &viewport);
        list_->RSSetScissorRects(1, &scissor);
        list_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        list_->DrawInstanced(3, 1, 0, 0);
    }
    return submit(queue, error);
}
}
