#include "hom_grade.h"
#include "Engine/Core/Log/logging.h"
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstring>

namespace dingosdk::overlay {
namespace {
using Microsoft::WRL::ComPtr;

// Full-screen triangle; the pixel shader reads the copy of the picture texel for texel.
constexpr char shader_source[] = R"(
cbuffer Grade : register(b0) { float strength; float saturation; float blue; float pad; };
Texture2D picture : register(t0);
float4 vs(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 ps(float4 position : SV_Position) : SV_Target {
    float3 c = picture.Load(int3(position.xy, 0)).rgb;
    float l = dot(c, float3(0.299, 0.587, 0.114));
    float3 graded = lerp(float3(l, l, l), c, saturation) * float3(1.0, 1.0, blue);
    return float4(lerp(c, graded, strength), 1.0);
}
)";

struct Pass {
    bool failed{};
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    ComPtr<ID3D12DescriptorHeap> heap;
    ComPtr<ID3D12Resource> copy;
    DXGI_FORMAT format{};
    UINT64 width{};
    UINT height{};
};
Pass &pass() {
    static Pass value;
    return value;
}

bool fail(const char *what, HRESULT result) {
    pass().failed = true;
    logging::log(logging::Level::warning, logging::Channel::graphics, "Hall Of Meat: colour pass unavailable ({}, {:#x}).", what,
                 static_cast<unsigned>(result));
    return false;
}

bool prepare(ID3D12Device *device, const D3D12_RESOURCE_DESC &target) {
    auto &p = pass();
    if (p.failed) return false;
    if (!p.root) {
        D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
        D3D12_ROOT_PARAMETER parameters[2]{};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[0].Constants = {0, 0, 4};
        parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[1].DescriptorTable = {1, &range};
        parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC description{2, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        ComPtr<ID3DBlob> blob, error;
        HRESULT result = D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
        if (FAILED(result)) return fail("root signature", result);
        result = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&p.root));
        if (FAILED(result)) return fail("root signature", result);
        D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        result = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&p.heap));
        if (FAILED(result)) return fail("descriptor heap", result);
    }
    if (!p.pipeline || p.format != target.Format) {
        ComPtr<ID3DBlob> vs, ps, error;
        HRESULT result = D3DCompile(shader_source, std::strlen(shader_source), "hom_grade", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vs, &error);
        if (SUCCEEDED(result)) result = D3DCompile(shader_source, std::strlen(shader_source), "hom_grade", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &ps, &error);
        if (FAILED(result)) return fail("shader", result);
        D3D12_GRAPHICS_PIPELINE_STATE_DESC state{};
        state.pRootSignature = p.root.Get();
        state.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
        state.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
        state.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        state.SampleMask = UINT_MAX;
        state.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        state.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        state.RasterizerState.DepthClipEnable = TRUE;
        state.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        state.NumRenderTargets = 1;
        state.RTVFormats[0] = target.Format;
        state.SampleDesc.Count = 1;
        p.pipeline.Reset();
        result = device->CreateGraphicsPipelineState(&state, IID_PPV_ARGS(&p.pipeline));
        if (FAILED(result)) return fail("pipeline", result);
    }
    if (!p.copy || p.format != target.Format || p.width != target.Width || p.height != target.Height) {
        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        description.Width = target.Width;
        description.Height = target.Height;
        description.DepthOrArraySize = 1;
        description.MipLevels = 1;
        description.Format = target.Format;
        description.SampleDesc.Count = 1;
        description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
        p.copy.Reset();
        const HRESULT result = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                               IID_PPV_ARGS(&p.copy));
        if (FAILED(result)) return fail("picture copy", result);
        device->CreateShaderResourceView(p.copy.Get(), nullptr, p.heap->GetCPUDescriptorHandleForHeapStart());
        p.format = target.Format;
        p.width = target.Width;
        p.height = target.Height;
    }
    return true;
}

void transition(ID3D12GraphicsCommandList *commands, ID3D12Resource *resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    commands->ResourceBarrier(1, &barrier);
}
} // namespace

std::atomic<float> &hom_colour_strength() noexcept {
    static std::atomic<float> value{};
    return value;
}

bool hom_grade_record(ID3D12Device *device, ID3D12GraphicsCommandList *commands, ID3D12Resource *buffer, D3D12_CPU_DESCRIPTOR_HANDLE rtv,
                      float strength) noexcept {
    try {
        if (!device || !commands || !buffer || !(strength > 0.01f)) return false;
        const auto target = buffer->GetDesc();
        if (target.SampleDesc.Count != 1 || !prepare(device, target)) return false;
        auto &p = pass();
        transition(commands, buffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
        commands->CopyResource(p.copy.Get(), buffer);
        transition(commands, p.copy.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        transition(commands, buffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        commands->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        const D3D12_VIEWPORT viewport{0, 0, static_cast<float>(target.Width), static_cast<float>(target.Height), 0, 1};
        const D3D12_RECT scissor{0, 0, static_cast<LONG>(target.Width), static_cast<LONG>(target.Height)};
        commands->RSSetViewports(1, &viewport);
        commands->RSSetScissorRects(1, &scissor);
        ID3D12DescriptorHeap *heaps[]{p.heap.Get()};
        commands->SetDescriptorHeaps(1, heaps);
        commands->SetGraphicsRootSignature(p.root.Get());
        commands->SetPipelineState(p.pipeline.Get());
        // Skate 3's colour_matrix_hall_of_meat: near and far saturation 0.5, multiply (1, 1, 1.2).
        const float constants[4]{std::min(strength, 1.0f), 0.5f, 1.2f, 0.0f};
        commands->SetGraphicsRoot32BitConstants(0, 4, constants, 0);
        commands->SetGraphicsRootDescriptorTable(1, p.heap->GetGPUDescriptorHandleForHeapStart());
        commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        commands->DrawInstanced(3, 1, 0, 0);
        transition(commands, p.copy.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        return true;
    } catch (...) {
        pass().failed = true;
        return false;
    }
}

void hom_grade_release() noexcept {
    auto &p = pass();
    p.copy.Reset();
    p.pipeline.Reset();
    p.heap.Reset();
    p.root.Reset();
    p.width = p.height = 0;
}
} // namespace dingosdk::overlay
