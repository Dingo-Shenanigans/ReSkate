#include "../material_mask_formats.h"
#include "../material_transmission.cpp"
#include <iostream>
using namespace dingosdk::overlay;
namespace
{
// Pipeline stream subobjects require pointer alignment, including tail padding.
#pragma warning(push)
#pragma warning(disable : 4324)
template <D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type, class T> struct alignas(void *) StreamPart
{
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type{Type};
    T value{};
};

#pragma warning(pop)
void test_mask_pipeline_formats(ID3D12Device *device)
{
    constexpr char source[] = R"(
float4 vs(uint id:SV_VertexID):SV_Position { return float4(id==1?3:-1,id==2?3:-1,0,1); }
struct Mask {float4 a:SV_Target0;float4 b:SV_Target1;float4 c:SV_Target2;uint4 d:SV_Target3;};
Mask ps(){Mask o;o.a=0;o.b=0;o.c=0;o.d=0;return o;}
)";
    ComPtr<ID3DBlob> vs, ps, error, bytes;
    check(D3DCompile(source, sizeof(source) - 1, nullptr, nullptr, nullptr, "vs", "vs_5_0",
                     D3DCOMPILE_WARNINGS_ARE_ERRORS, 0, &vs, &error));
    check(D3DCompile(source, sizeof(source) - 1, nullptr, nullptr, nullptr, "ps", "ps_5_0",
                     D3DCOMPILE_WARNINGS_ARE_ERRORS, 0, &ps, &error));
    D3D12_ROOT_SIGNATURE_DESC desc{};
    check(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &bytes, &error));
    ComPtr<ID3D12RootSignature> root;
    check(device->CreateRootSignature(0, bytes->GetBufferPointer(), bytes->GetBufferSize(), IID_PPV_ARGS(&root)));
    D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};
    p.pRootSignature = root.Get();
    p.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    p.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    p.SampleMask = UINT_MAX;
    p.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    p.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    p.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    p.SampleDesc.Count = 1;
    const auto formats = detail::material_mask_formats();
    p.NumRenderTargets = formats.NumRenderTargets;
    std::copy_n(formats.RTFormats, 8, p.RTVFormats);
    for (auto &target : p.BlendState.RenderTarget)
        target.RenderTargetWriteMask = 15;
    // Reproduce the native five-to-four reduction: an unused fifth format is invalid.
    p.RTVFormats[4] = DXGI_FORMAT_R8_UINT;
    ComPtr<ID3D12PipelineState> pipeline;
    if (SUCCEEDED(device->CreateGraphicsPipelineState(&p, IID_PPV_ARGS(&pipeline))))
        throw std::runtime_error("stale format unexpectedly accepted");
    std::copy_n(formats.RTFormats, 8, p.RTVFormats);
    check(device->CreateGraphicsPipelineState(&p, IID_PPV_ARGS(&pipeline)));
    struct Stream
    {
        StreamPart<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature *> root;
        StreamPart<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, D3D12_SHADER_BYTECODE> vs;
        StreamPart<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE> ps;
        StreamPart<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC> raster;
        StreamPart<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL, D3D12_DEPTH_STENCIL_DESC> depth;
        StreamPart<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY, D3D12_PRIMITIVE_TOPOLOGY_TYPE> topology;
        StreamPart<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY> formats;
        StreamPart<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, DXGI_SAMPLE_DESC> samples;
    } stream;
    stream.root.value = p.pRootSignature;
    stream.vs.value = p.VS;
    stream.ps.value = p.PS;
    stream.raster.value = p.RasterizerState;
    stream.depth.value = p.DepthStencilState;
    stream.topology.value = p.PrimitiveTopologyType;
    stream.formats.value = formats;
    stream.samples.value = p.SampleDesc;
    ComPtr<ID3D12Device2> device2;
    check(device->QueryInterface(IID_PPV_ARGS(&device2)));
    D3D12_PIPELINE_STATE_STREAM_DESC stream_desc{sizeof(stream), &stream};
    pipeline.Reset();
    check(device2->CreatePipelineState(&stream_desc, IID_PPV_ARGS(&pipeline)));
}
} // namespace
int main(int argc, char **argv)
{
    try
    {
        const std::string scenario = argc > 1 ? argv[1] : "tint";
        const bool wrong_view = scenario == "wrong-view", hdr = scenario == "hdr", stale = scenario == "stale";
        const bool refract = scenario == "refraction", straight = scenario == "straight",
                   foreground = scenario == "foreground";
        set_material_transmission_enabled(true);
        ComPtr<ID3D12Device> device;
        check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
        if (scenario == "pipeline-formats")
        {
            test_mask_pipeline_formats(device.Get());
            return 0;
        }
        ComPtr<ID3D12CommandQueue> queue;
        D3D12_COMMAND_QUEUE_DESC q{};
        check(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)));
        auto colour = make_resource(
            device.Get(), image_desc(64, 64, DXGI_FORMAT_R10G10B10A2_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET),
            D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_PRESENT);
        present_material_transmission(device.Get(), queue.Get(), colour.Get(), 0);
        auto depth = make_resource(
            device.Get(), image_desc(64, 64, DXGI_FORMAT_R32G8X24_TYPELESS, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL),
            D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        auto other_depth = make_resource(
            device.Get(), image_desc(64, 64, DXGI_FORMAT_R32G8X24_TYPELESS, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL),
            D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        ComPtr<ID3D12DescriptorHeap> dsv;
        D3D12_DESCRIPTOR_HEAP_DESC h{};
        h.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        h.NumDescriptors = 1;
        check(device->CreateDescriptorHeap(&h, IID_PPV_ARGS(&dsv)));
        auto dh = dsv->GetCPUDescriptorHandleForHeapStart();
        D3D12_DEPTH_STENCIL_VIEW_DESC dv{};
        dv.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        device->CreateDepthStencilView(depth.Get(), &dv, dh);
        material_depth_descriptor(depth.Get(), &dv, dh);
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> commands;
        check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
        check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                        IID_PPV_ARGS(&commands)));
        commands->ClearDepthStencilView(dh, D3D12_CLEAR_FLAG_DEPTH, foreground ? .9f : .01f, 0, 0, nullptr);
        MaterialMaskDraw draw{};
        draw.device = device.Get();
        draw.commands = commands.Get();
        draw.target_count = 4;
        draw.formats.fill(DXGI_FORMAT_UNKNOWN);
        for (UINT i = 0; i < 4; ++i)
            draw.formats[i] = DXGI_FORMAT_R32G32B32A32_FLOAT;
        draw.viewport = {0, 0, 64, 64, 0, 1};
        draw.depth = dh;
        if (refract || straight)
            draw.parameters = {0, 0, 0, straight ? 1.f : 1.52f, 12, 0, 0, 0};
        draw.replay = [&](D3D12_CPU_DESCRIPTOR_HANDLE first) {
            std::array<std::array<float, 4>, 4> values{
                {{0, 0, 2, 1}, {.35f, 0, -.93675f, .006f}, {0, 0, 0, 0}, {0, .1f, 1, -1}}};
            if (refract || straight)
            {
                values[0] = {0, 0, .1f, 1};
                values[1] = {.7f, 0, -.7141428f, .03f};
            }
            auto stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            for (UINT i = 0; i < 4; ++i)
            {
                auto handle = first;
                handle.ptr += i * stride;
                commands->ClearRenderTargetView(handle, values[i].data(), 0, nullptr);
            }
        };
        record_material_mask(draw);
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {depth.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                              D3D12_RESOURCE_STATE_DEPTH_READ};
        if (wrong_view)
            barrier.Transition.pResource = other_depth.Get();
        auto original = +[](ID3D12GraphicsCommandList *list, UINT count, const D3D12_RESOURCE_BARRIER *barriers) {
            list->ResourceBarrier(count, barriers);
        };
        material_depth_barriers(commands.Get(), 1, &barrier, original);
        commands->ResourceBarrier(1, &barrier);
        ComPtr<ID3D12DescriptorHeap> rtv;
        h.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        check(device->CreateDescriptorHeap(&h, IID_PPV_ARGS(&rtv)));
        auto ch = rtv->GetCPUDescriptorHandleForHeapStart();
        device->CreateRenderTargetView(colour.Get(), nullptr, ch);
        transition(commands.Get(), colour.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const float background[]{.7f, .5f, .3f, 1};
        commands->ClearRenderTargetView(ch, background, 0, nullptr);
        if (refract || straight)
            for (LONG x = 0; x < 64; ++x)
            {
                const float pixel[]{.1f + .8f * x / 63, .5f, .3f, 1};
                const D3D12_RECT rect{x, 0, x + 1, 64};
                commands->ClearRenderTargetView(ch, pixel, 1, &rect);
            }
        transition(commands.Get(), colour.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        check(commands->Close());
        ID3D12CommandList *lists[]{commands.Get()};
        queue->ExecuteCommandLists(1, lists);
        material_queue_submission(queue.Get(), 1, lists);
        present_material_transmission(device.Get(), queue.Get(), colour.Get(), hdr ? 1 : 0);
        auto &s = state();
        if (s.failed || s.masks[0].used == (wrong_view || hdr))
            throw std::runtime_error("live compositor did not submit");
        if (stale)
        {
            present_material_transmission(device.Get(), queue.Get(), colour.Get(), 0);
            present_material_transmission(device.Get(), queue.Get(), colour.Get(), 0);
        }
        ComPtr<ID3D12Fence> fence;
        check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
        HANDLE event{};
        check(queue->Signal(fence.Get(), 1));
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        check(fence->SetEventOnCompletion(1, event));
        if (WaitForSingleObject(event, 15000) != WAIT_OBJECT_0)
            throw std::runtime_error("source completion timeout");
        CloseHandle(event);
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
        UINT64 size{};
        auto shape = colour->GetDesc();
        device->GetCopyableFootprints(&shape, 0, 1, 0, &layout, nullptr, nullptr, &size);
        auto readback =
            make_resource(device.Get(), buffer_desc(size), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        check(allocator->Reset());
        check(commands->Reset(allocator.Get(), nullptr));
        transition(commands.Get(), colour.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION to{}, from{};
        to.pResource = readback.Get();
        to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        to.PlacedFootprint = layout;
        from.pResource = colour.Get();
        from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        commands->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        transition(commands.Get(), colour.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
        check(commands->Close());
        queue->ExecuteCommandLists(1, lists);
        check(queue->Signal(fence.Get(), 3));
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        check(fence->SetEventOnCompletion(3, event));
        if (WaitForSingleObject(event, 15000) != WAIT_OBJECT_0)
            throw std::runtime_error("output timeout");
        CloseHandle(event);
        void *pixels{};
        check(readback->Map(0, nullptr, &pixels));
        const auto packed = *reinterpret_cast<const UINT *>(static_cast<const char *>(pixels) +
                                                            32 * layout.Footprint.RowPitch + 32 * 4);
        readback->Unmap(0, nullptr);
        const float red = static_cast<float>(packed & 1023) / 1023;
        const bool correct = refract                             ? red > .3f && red < .48f
                             : straight                          ? red > .49f && red < .52f
                             : (wrong_view || hdr || foreground) ? red > .69f && red < .71f
                                                                 : red > .55f && red < .68f;
        if (!correct)
            throw std::runtime_error("live output did not tint the background");
        std::cout << scenario << ": red=" << red << '\n';
        material_copy_depth_descriptors(device.Get(), 1, {100000}, dh);
        check(queue->Signal(fence.Get(), 4));
        if (state().descriptors.size() > 8)
            throw std::runtime_error("descriptor retention is not bounded");
        std::cout << "PASS: production surface/depth/queue/presentation chain tinted synthetic D32S8/R10 input; fenced "
                     "output readback verified. No game files.\n";
        return 0;
    }
    catch (const std::exception &e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
