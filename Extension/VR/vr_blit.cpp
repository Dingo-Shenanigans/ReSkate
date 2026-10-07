#include "vr_blit.h"
#include <d3dcompiler.h>
#include <cstring>
#include <format>

namespace dingosdk::vr::detail {
using Microsoft::WRL::ComPtr;
namespace {
// Full-screen triangle. mode 0: the source holds gamma-encoded values (8- and
// 10-bit UNORM back buffers); mode 1: the source is linear (scRGB FP16).
constexpr char shader_source[] = R"(
Texture2D source : register(t0);
SamplerState linear_clamp : register(s0);
cbuffer Constants : register(b0) { uint mode; float u_offset; float u_scale; float vignette; };
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
// Comfort vignette: darkens towards the edges by `amount` (0 none, 1 a narrow window). The
// image is rendered wider than the lenses show, so it starts well inside the image edge.
float3 comfort(float3 c, float2 uv, float amount) {
    if (amount <= 0) return c;
    float r = length((uv - 0.5) * 2);
    float inner = lerp(0.95, 0.15, amount);
    return c * (1 - smoothstep(inner, inner + 0.35, r));
}
float4 ps_main(Vertex v) : SV_Target {
    // u_offset/u_scale pick a horizontal window of the source (side-by-side halves).
    float3 c = saturate(source.SampleLevel(linear_clamp, float2(u_offset + v.uv.x * u_scale, v.uv.y), 0).rgb);
    if (mode == 0) c = srgb_to_linear(c);
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
    const HRESULT result = compiler(shader_source, sizeof(shader_source) - 1, "reskate_vr_blit", nullptr, nullptr, entry,
        target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &out, &messages);
    if (SUCCEEDED(result)) return true;
    error = hresult("Compiling the VR copy shader", result);
    if (messages) error += std::string(" ") + static_cast<const char*>(messages->GetBufferPointer());
    return false;
}
}

bool Blitter::supported_source(DXGI_FORMAT format) noexcept {
    return format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM ||
        format == DXGI_FORMAT_R10G10B10A2_UNORM || format == DXGI_FORMAT_R16G16B16A16_FLOAT;
}

bool Blitter::create(ID3D12Device* device, std::string& error) {
    destroy();
    const auto compiler = system_function<pD3DCompile>(L"d3dcompiler_47.dll", "D3DCompile");
    const auto serialize = system_function<PFN_D3D12_SERIALIZE_ROOT_SIGNATURE>(L"d3d12.dll", "D3D12SerializeRootSignature");
    if (!compiler || !serialize) { error = "d3dcompiler_47.dll or d3d12.dll is unavailable."; return false; }
    if (!compile(compiler, "vs_main", "vs_5_0", vertex_, error) || !compile(compiler, "ps_main", "ps_5_0", pixel_, error))
        return false;

    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    std::array<D3D12_ROOT_PARAMETER, 2> parameters{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].DescriptorTable = {1, &range};
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[1].Constants = {0, 0, 4};
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
    if (FAILED(result)) { error = hresult("Serializing the VR root signature", result); return false; }
    result = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root_));
    if (FAILED(result)) { error = hresult("Creating the VR root signature", result); return false; }

    D3D12_DESCRIPTOR_HEAP_DESC heap{};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors = slots;
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    result = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&srvs_));
    if (FAILED(result)) { error = hresult("Creating the VR SRV heap", result); return false; }
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    result = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&rtvs_));
    if (FAILED(result)) { error = hresult("Creating the VR RTV heap", result); return false; }
    srv_stride_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    rtv_stride_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for (auto& allocator : allocators_) {
        result = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
        if (FAILED(result)) { error = hresult("Creating a VR command allocator", result); return false; }
    }
    result = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators_[0].Get(), nullptr, IID_PPV_ARGS(&list_));
    if (FAILED(result)) { error = hresult("Creating the VR command list", result); return false; }
    list_->Close();
    list_->SetName(L"ReSkate VR copy");
    result = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_));
    if (FAILED(result)) { error = hresult("Creating the VR fence", result); return false; }
    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event_) { error = "Creating the VR fence event failed."; return false; }
    device_ = device;
    return true;
}

bool Blitter::wait(UINT64 value, DWORD timeout_ms) noexcept {
    if (!value || !fence_) return true;
    const auto done = fence_->GetCompletedValue();
    if (done == UINT64_MAX) return false; // device removed
    if (done >= value) return true;
    if (FAILED(fence_->SetEventOnCompletion(value, event_))) return false;
    if (WaitForSingleObject(event_, timeout_ms) != WAIT_OBJECT_0) return false;
    return fence_->GetCompletedValue() >= value;
}

bool Blitter::wait_idle(DWORD timeout_ms) noexcept { return wait(next_fence_, timeout_ms); }

void Blitter::destroy() noexcept {
    if (fence_) (void)wait_idle(2000);
    pipelines_.clear();
    list_.Reset();
    for (auto& allocator : allocators_) allocator.Reset();
    slot_fences_ = {};
    srvs_.Reset(); rtvs_.Reset(); root_.Reset(); vertex_.Reset(); pixel_.Reset(); fence_.Reset();
    if (event_) CloseHandle(event_);
    event_ = nullptr;
    next_fence_ = 0;
    slot_ = 0;
    device_.Reset();
}

ID3D12PipelineState* Blitter::pipeline(DXGI_FORMAT format, std::string& error) {
    if (const auto found = pipelines_.find(static_cast<int>(format)); found != pipelines_.end()) return found->second.Get();
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = root_.Get();
    desc.VS = {vertex_->GetBufferPointer(), vertex_->GetBufferSize()};
    desc.PS = {pixel_->GetBufferPointer(), pixel_->GetBufferSize()};
    // Every enum gets a valid value, even where blending and depth are off.
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
    if (FAILED(result)) { error = hresult("Creating the VR copy pipeline", result); return nullptr; }
    return (pipelines_[static_cast<int>(format)] = state).Get();
}

bool Blitter::copy(ID3D12CommandQueue* queue, ID3D12Resource* source, ID3D12Resource* target, DXGI_FORMAT target_format,
    std::string& error, float u_offset, float u_scale, float vignette) {
    if (!device_) { error = "The VR copy is not initialised."; return false; }
    const auto source_desc = source->GetDesc();
    const auto target_desc = target->GetDesc();
    if (!supported_source(source_desc.Format)) {
        error = std::format("The game image format {} is not supported for VR.", static_cast<int>(source_desc.Format));
        return false;
    }
    auto* state = pipeline(target_format, error);
    if (!state) return false;
    // Reuse this slot's allocator and descriptors only once its last copy finished.
    if (!wait(slot_fences_[slot_], 1000)) { error = "The GPU did not finish an earlier VR copy."; return false; }
    auto& allocator = allocators_[slot_];
    HRESULT result = allocator->Reset();
    if (SUCCEEDED(result)) result = list_->Reset(allocator.Get(), state);
    if (FAILED(result)) { error = hresult("Resetting the VR command list", result); return false; }

    auto srv = srvs_->GetCPUDescriptorHandleForHeapStart();
    srv.ptr += static_cast<SIZE_T>(slot_) * srv_stride_;
    auto srv_gpu = srvs_->GetGPUDescriptorHandleForHeapStart();
    srv_gpu.ptr += static_cast<UINT64>(slot_) * srv_stride_;
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = source_desc.Format;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Texture2D.MipLevels = 1;
    device_->CreateShaderResourceView(source, &view, srv);
    auto rtv = rtvs_->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(slot_) * rtv_stride_;
    D3D12_RENDER_TARGET_VIEW_DESC target_view{};
    target_view.Format = target_format;
    target_view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    device_->CreateRenderTargetView(target, &target_view, rtv);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = source;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    list_->ResourceBarrier(1, &barrier);
    ID3D12DescriptorHeap* heaps[] = {srvs_.Get()};
    list_->SetDescriptorHeaps(1, heaps);
    list_->SetGraphicsRootSignature(root_.Get());
    list_->SetGraphicsRootDescriptorTable(0, srv_gpu);
    const UINT mode = source_desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 1u : 0u;
    std::array<UINT, 4> constants{mode, 0, 0, 0};
    std::memcpy(&constants[1], &u_offset, sizeof(float));
    std::memcpy(&constants[2], &u_scale, sizeof(float));
    std::memcpy(&constants[3], &vignette, sizeof(float));
    list_->SetGraphicsRoot32BitConstants(1, static_cast<UINT>(constants.size()), constants.data(), 0);
    const D3D12_VIEWPORT viewport{0, 0, static_cast<float>(target_desc.Width), static_cast<float>(target_desc.Height), 0, 1};
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(target_desc.Width), static_cast<LONG>(target_desc.Height)};
    list_->RSSetViewports(1, &viewport);
    list_->RSSetScissorRects(1, &scissor);
    list_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list_->DrawInstanced(3, 1, 0, 0);
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    list_->ResourceBarrier(1, &barrier);
    result = list_->Close();
    if (FAILED(result)) { error = hresult("Closing the VR command list", result); return false; }
    ID3D12CommandList* lists[] = {list_.Get()};
    queue->ExecuteCommandLists(1, lists);
    const UINT64 signal = ++next_fence_;
    result = queue->Signal(fence_.Get(), signal);
    if (FAILED(result)) { error = hresult("Signalling the VR fence", result); return false; }
    slot_fences_[slot_] = signal;
    slot_ = (slot_ + 1) % slots;
    return true;
}
}
