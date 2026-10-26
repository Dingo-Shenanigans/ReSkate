#include "material_capture.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Vfs/mod_catalog.h"
#include "material_mask_formats.h"
#include "material_method_hooks.h"
#include "material_shader_identity.h"
#include "material_transmission.h"
#include "transmission_config.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <vector>
#include <wrl/client.h>
namespace dingosdk::overlay
{
namespace detail
{
void install_depth_probe(ID3D12GraphicsCommandList *) noexcept;
}
namespace
{
using Microsoft::WRL::ComPtr;
thread_local bool private_pipeline_build{}, private_commands{};
struct InternalScope
{
    bool &flag;
    bool previous;
    explicit InternalScope(bool &value) : flag(value), previous(value)
    {
        flag = true;
    }
    ~InternalScope()
    {
        flag = previous;
    }
};

using Make = HRESULT(STDMETHODCALLTYPE *)(ID3D12Device *, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *, REFIID, void **);
using Set = void(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *, ID3D12PipelineState *);
using Reset = HRESULT(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *, ID3D12CommandAllocator *,
                                           ID3D12PipelineState *);
using Targets = void(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE *, BOOL,
                                          const D3D12_CPU_DESCRIPTOR_HANDLE *);
using Viewports = void(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *, UINT, const D3D12_VIEWPORT *);
using DrawPlain = void(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *, UINT, UINT, UINT, UINT);
using Draw = void(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *, UINT, UINT, UINT, INT, UINT);
using Execute = void(STDMETHODCALLTYPE *)(ID3D12CommandQueue *, UINT, ID3D12CommandList *const *);
using MakeStream = HRESULT(STDMETHODCALLTYPE *)(ID3D12Device2 *, const D3D12_PIPELINE_STATE_STREAM_DESC *, REFIID,
                                                void **);
detail::material_method_hooks<MakeStream, 47> original_stream;
detail::material_method_hooks<Make, 10> original_make;
detail::material_method_hooks<Set, 25> original_set;
detail::material_method_hooks<Set, 11> original_clear;
detail::material_method_hooks<Reset, 10> original_reset;
detail::material_method_hooks<Targets, 46> original_targets;
detail::material_method_hooks<Viewports, 21> original_viewports;
detail::material_method_hooks<Draw, 13> original_draw;
detail::material_method_hooks<DrawPlain, 12> original_plain;
detail::material_method_hooks<Execute, 10> original_execute;
using Indirect = void(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *, ID3D12CommandSignature *, UINT,
                                           ID3D12Resource *, UINT64, ID3D12Resource *, UINT64);
detail::material_method_hooks<Indirect, 59> original_indirect;
using Recipe = detail::TransmissionRecipe;
struct Pipeline
{
    ComPtr<ID3D12PipelineState> source, mask;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    std::array<float, 8> parameters{};
    UINT native_targets{};
    ComPtr<ID3DBlob> cached;
};
struct ListState
{
    ID3D12PipelineState *pipeline{};
    UINT count{};
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 8> targets{};
    D3D12_CPU_DESCRIPTOR_HANDLE depth{};
    bool has_depth{};
    D3D12_VIEWPORT viewport{};
};
struct Capture
{
    std::mutex mutex;
    ComPtr<ID3D12Device> device;
    std::vector<Recipe> recipes;
    std::map<ID3D12PipelineState *, Pipeline> pipelines;
    std::map<ID3D12GraphicsCommandList *, ListState> lists;
    std::atomic<bool> failed{};
};
Capture &state()
{
    static auto *p = new Capture;
    return *p;
}
const std::vector<detail::TransmissionRecipe> &configured_recipes()
{
    static const auto recipes = [] {
        std::vector<detail::TransmissionRecipe> result;
        for (const auto &mod : mods::catalog().mods)
            try
            {
                const auto duplicates =
                    detail::append_transmission_recipes(result, detail::read_transmission_config(mod.directory));
                if (duplicates)
                    logging::log(logging::Level::warning, logging::Channel::graphics,
                                 "Material transmission: {} declares {} already-owned "
                                 "shader(s); higher-priority declarations retained.",
                                 mod.name, duplicates);
            }
            catch (const std::exception &e)
            {
                logging::log(logging::Level::warning, logging::Channel::graphics, "Material transmission: {}: {}",
                             mod.name, e.what());
            }
        return result;
    }();
    return recipes;
}
ListState &tracked_list(Capture &s, ID3D12GraphicsCommandList *list)
{
    if (!s.lists.contains(list) && s.lists.size() >= 8192)
    {
        s.lists.erase(s.lists.begin());
    }
    return s.lists[list];
}
void say(const char *message);
// COM wrappers may expose a different interface pointer for the same pipeline.
// Object-private metadata follows the object instead of relying on pointer
// identity.
constexpr GUID pipeline_identity{0x16501c78, 0xc5a1, 0x4a49, {0xb8, 0xa1, 0x84, 0xd1, 0x3d, 0xf9, 0x90, 0xa6}};
void mark_pipeline(ID3D12PipelineState *p)
{
    const auto key = reinterpret_cast<UINT64>(p);
    if (FAILED(p->SetPrivateData(pipeline_identity, sizeof(key), &key)))
        say("Material capture: pipeline object metadata was refused.");
}
auto find_pipeline(Capture &s, ID3D12PipelineState *p)
{
    auto found = s.pipelines.find(p);
    if (found != s.pipelines.end() || !p)
        return found;
    UINT64 key{};
    UINT bytes = sizeof(key);
    if (SUCCEEDED(p->GetPrivateData(pipeline_identity, &bytes, &key)) && bytes == sizeof(key))
        found = s.pipelines.find(reinterpret_cast<ID3D12PipelineState *>(key));
    if (found == s.pipelines.end())
    {
        static std::map<ID3D12PipelineState *, ID3D12PipelineState *> aliases;
        if (const auto known = aliases.find(p); known != aliases.end())
            return s.pipelines.find(known->second);
        if (aliases.size() < 4096)
        {
            ComPtr<ID3DBlob> cached;
            if (SUCCEEDED(p->GetCachedBlob(&cached)) && cached && cached->GetBufferSize() <= 8 * 1024 * 1024)
            {
                for (auto candidate = s.pipelines.begin(); candidate != s.pipelines.end(); ++candidate)
                    if (candidate->second.cached &&
                        candidate->second.cached->GetBufferSize() == cached->GetBufferSize() &&
                        !std::memcmp(candidate->second.cached->GetBufferPointer(), cached->GetBufferPointer(),
                                     cached->GetBufferSize()))
                    {
                        found = candidate;
                        say("Material capture: resolved a cached native pipeline "
                            "identity.");
                        break;
                    }
            }
            aliases.emplace(p, found == s.pipelines.end() ? nullptr : found->first);
        }
    }
    return found;
}
void say(const char *message)
{
    logging::write(logging::Level::info, logging::Channel::graphics, message);
}
HRESULT STDMETHODCALLTYPE make(ID3D12Device *d, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc, REFIID iid,
                               void **output)
{
    static std::atomic<unsigned> calls{};
    if (calls.fetch_add(1) == 0)
        say("Material capture: observing live graphics pipeline descriptions.");
    const auto fn = original_make.load();
    const auto hr = fn(d, desc, iid, output);
    if (private_pipeline_build || private_commands)
        return hr;
    if (FAILED(hr) || !output || !*output || !desc)
        return hr;
    try
    {
        auto &s = state();
        std::lock_guard lock(s.mutex);
        if (s.failed || (!material_transmission_requested()) || s.pipelines.size() >= 64 ||
            desc->SampleDesc.Count != 1 || desc->NumRenderTargets < 1 || desc->NumRenderTargets > 8)
            return hr;
        const auto shader_key = detail::transmission_shader_key(
            {static_cast<const char *>(desc->PS.pShaderBytecode), desc->PS.BytecodeLength});
        for (const auto &recipe : s.recipes)
        {
            if (shader_key != recipe.shader)
                continue;
            auto edited = *desc;
            const auto formats = detail::material_mask_formats();
            edited.NumRenderTargets = formats.NumRenderTargets;
            std::copy_n(formats.RTFormats, 8, edited.RTVFormats);
            edited.PS = {recipe.mask.data(), recipe.mask.size()};
            edited.CachedPSO = {};
            edited.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
            edited.DepthStencilState.StencilEnable = FALSE;
            edited.BlendState.AlphaToCoverageEnable = FALSE;
            edited.BlendState.IndependentBlendEnable = TRUE;
            for (UINT i = 0; i < edited.NumRenderTargets; ++i)
            {
                edited.BlendState.RenderTarget[i].BlendEnable = FALSE;
                edited.BlendState.RenderTarget[i].LogicOpEnable = FALSE;
                edited.BlendState.RenderTarget[i].RenderTargetWriteMask = 15;
            }
            Pipeline p;
            p.parameters = recipe.parameters;
            p.source = static_cast<ID3D12PipelineState *>(*output);
            p.native_targets = desc->NumRenderTargets;
            p.desc = edited;
            (void)static_cast<ID3D12PipelineState *>(*output)->GetCachedBlob(&p.cached);
            HRESULT built{};
            {
                InternalScope internal(private_pipeline_build);
                built = fn(d, &edited, IID_PPV_ARGS(&p.mask));
            }
            if (SUCCEEDED(built))
            {
                s.pipelines.emplace(static_cast<ID3D12PipelineState *>(*output), std::move(p));
                mark_pipeline(static_cast<ID3D12PipelineState *>(*output));
                say("Material capture: matched native pixel bytecode; private mask "
                    "pipeline ready.");
            }
            else
            {
                say("Material capture: mask pipeline rejected; native pipeline "
                    "unchanged.");
            }
            break;
        }
    }
    catch (...)
    {
        say("Material capture: pipeline observation failed; native pipeline "
            "unchanged.");
    }
    return hr;
}
size_t stream_size(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type)
{
    switch (type)
    {
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE:
        return sizeof(ID3D12RootSignature *);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
        return sizeof(D3D12_SHADER_BYTECODE);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS:
        return sizeof(D3D12_SHADER_BYTECODE);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL2:
        return sizeof(D3D12_DEPTH_STENCIL_DESC2);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER1:
        return sizeof(D3D12_RASTERIZER_DESC1);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER2:
        return sizeof(D3D12_RASTERIZER_DESC2);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT:
        return sizeof(D3D12_STREAM_OUTPUT_DESC);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND:
        return sizeof(D3D12_BLEND_DESC);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS:
        return 4;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER:
        return sizeof(D3D12_RASTERIZER_DESC);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL:
        return sizeof(D3D12_DEPTH_STENCIL_DESC);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1:
        return sizeof(D3D12_DEPTH_STENCIL_DESC1);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT:
        return sizeof(D3D12_INPUT_LAYOUT_DESC);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS:
        return sizeof(D3D12_RT_FORMAT_ARRAY);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC:
        return sizeof(DXGI_SAMPLE_DESC);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO:
        return sizeof(D3D12_CACHED_PIPELINE_STATE);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING:
        return sizeof(D3D12_VIEW_INSTANCING_DESC);
    default:
        return 0;
    }
}
HRESULT observe_stream(ID3D12Device2 *d, const D3D12_PIPELINE_STATE_STREAM_DESC *desc, void *output)
{
    const auto fn = original_stream.load(d);
    const auto hr = S_OK;
    if (private_pipeline_build || private_commands)
        return hr;
    if (!output || !desc || !desc->pPipelineStateSubobjectStream || desc->SizeInBytes > 65536)
        return hr;
    try
    {
        auto &s = state();
        std::lock_guard lock(s.mutex);
        if (s.failed || (!material_transmission_requested()) || s.pipelines.size() >= 64)
            return hr;
        const auto *bytes = static_cast<const std::byte *>(desc->pPipelineStateSubobjectStream);
        std::map<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE, size_t> fields;
        for (size_t at = 0; at < desc->SizeInBytes;)
        {
            if (desc->SizeInBytes - at < 4)
                return hr;
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type{};
            std::memcpy(&type, bytes + at, 4);
            const auto size = stream_size(type);
            if (!size)
            {
                static std::atomic<int> reported{-1};
                if (reported.exchange(static_cast<int>(type)) != static_cast<int>(type))
                    logging::printf(logging::Level::info, logging::Channel::graphics,
                                    "Material capture: unsupported stream subobject %d.", static_cast<int>(type));
                return hr;
            }
            // Stream payloads align to their C++ type: pointer-bearing payloads to8,
            // scalar structs to4.
            const bool eight =
                type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS || type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS ||
                type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE ||
                type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS || type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS ||
                type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS || type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS ||
                type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS || type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS ||
                type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT ||
                type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT ||
                type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO ||
                type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING;
            const auto payload = (at + 4 + (eight ? 7 : 3)) & ~size_t(eight ? 7 : 3);
            if (payload > desc->SizeInBytes || size > desc->SizeInBytes - payload || fields.contains(type))
                return hr;
            fields[type] = payload;
            at = (payload + size + 7) & ~size_t(7);
        }
        const auto ps = fields.find(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS);
        const auto rt = fields.find(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS);
        if (ps == fields.end() || rt == fields.end())
            return hr;
        D3D12_SHADER_BYTECODE pixel{};
        std::memcpy(&pixel, bytes + ps->second, sizeof(pixel));
        if (!fields.contains(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL) &&
            !fields.contains(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1) &&
            !fields.contains(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL2))
            return hr;
        const auto shader_key =
            detail::transmission_shader_key({static_cast<const char *>(pixel.pShaderBytecode), pixel.BytecodeLength});
        for (const auto &recipe : s.recipes)
        {
            if (shader_key != recipe.shader)
                continue;
            auto copy = std::vector<std::byte>(bytes, bytes + desc->SizeInBytes);
            pixel = {recipe.mask.data(), recipe.mask.size()};
            std::memcpy(copy.data() + ps->second, &pixel, sizeof(pixel));
            D3D12_RT_FORMAT_ARRAY formats{};
            std::memcpy(&formats, bytes + rt->second, sizeof(formats));
            if (formats.NumRenderTargets < 1 || formats.NumRenderTargets > 8)
                return hr;
            const auto native_targets = formats.NumRenderTargets;
            formats = detail::material_mask_formats();
            std::memcpy(copy.data() + rt->second, &formats, sizeof(formats));
            for (const auto &[type, offset] : fields)
            {
                if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL)
                {
                    D3D12_DEPTH_STENCIL_DESC x{};
                    std::memcpy(&x, bytes + offset, sizeof(x));
                    x.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
                    x.StencilEnable = FALSE;
                    std::memcpy(copy.data() + offset, &x, sizeof(x));
                }
                else if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1)
                {
                    D3D12_DEPTH_STENCIL_DESC1 x{};
                    std::memcpy(&x, bytes + offset, sizeof(x));
                    x.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
                    x.StencilEnable = FALSE;
                    std::memcpy(copy.data() + offset, &x, sizeof(x));
                }
                else if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL2)
                {
                    D3D12_DEPTH_STENCIL_DESC2 x{};
                    std::memcpy(&x, bytes + offset, sizeof(x));
                    x.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
                    x.StencilEnable = FALSE;
                    std::memcpy(copy.data() + offset, &x, sizeof(x));
                }
                else if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND)
                {
                    D3D12_BLEND_DESC x{};
                    std::memcpy(&x, bytes + offset, sizeof(x));
                    x.AlphaToCoverageEnable = FALSE;
                    x.IndependentBlendEnable = TRUE;
                    for (auto &t : x.RenderTarget)
                    {
                        t.BlendEnable = FALSE;
                        t.LogicOpEnable = FALSE;
                        t.RenderTargetWriteMask = 15;
                    }
                    std::memcpy(copy.data() + offset, &x, sizeof(x));
                }
                else if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO)
                {
                    D3D12_CACHED_PIPELINE_STATE x{};
                    std::memcpy(copy.data() + offset, &x, sizeof(x));
                }
            }
            Pipeline p;
            p.parameters = recipe.parameters;
            p.native_targets = native_targets;
            p.source = static_cast<ID3D12PipelineState *>(output);
            (void)static_cast<ID3D12PipelineState *>(output)->GetCachedBlob(&p.cached);
            p.desc.NumRenderTargets = formats.NumRenderTargets;
            std::copy_n(formats.RTFormats, 8, p.desc.RTVFormats);
            p.desc.SampleDesc.Count = 1;
            if (const auto samples = fields.find(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC);
                samples != fields.end())
                std::memcpy(&p.desc.SampleDesc, bytes + samples->second, sizeof(DXGI_SAMPLE_DESC));
            if (p.desc.SampleDesc.Count != 1)
                return hr;
            D3D12_PIPELINE_STATE_STREAM_DESC edited{copy.size(), copy.data()};
            HRESULT built{};
            {
                InternalScope internal(private_pipeline_build);
                built = fn(d, &edited, IID_PPV_ARGS(&p.mask));
            }
            if (SUCCEEDED(built))
            {
                s.pipelines.emplace(static_cast<ID3D12PipelineState *>(output), std::move(p));
                mark_pipeline(static_cast<ID3D12PipelineState *>(output));
                say("Material capture: matched pipeline stream; private mask pipeline "
                    "ready.");
            }
            else
                logging::printf(logging::Level::warning, logging::Channel::graphics,
                                "Material capture: mask stream rejected (HRESULT 0x%08lx); native pipeline preserved.",
                                static_cast<unsigned long>(built));
            break;
        }
    }
    catch (...)
    {
        say("Material capture: stream observation failed; native pipeline "
            "unchanged.");
    }
    return hr;
}
HRESULT STDMETHODCALLTYPE make_stream(ID3D12Device2 *d, const D3D12_PIPELINE_STATE_STREAM_DESC *desc, REFIID iid,
                                      void **output)
{
    static std::atomic<unsigned> calls{};
    if (calls.fetch_add(1) == 0)
        say("Material capture: observing live pipeline-state streams.");
    const auto hr = original_stream.load()(d, desc, iid, output);
    if (SUCCEEDED(hr) && output && *output)
        (void)observe_stream(d, desc, *output);
    return hr;
}
using MakeLibrary = HRESULT(STDMETHODCALLTYPE *)(ID3D12Device1 *, const void *, SIZE_T, REFIID, void **);
using LoadStream = HRESULT(STDMETHODCALLTYPE *)(ID3D12PipelineLibrary1 *, LPCWSTR,
                                                const D3D12_PIPELINE_STATE_STREAM_DESC *, REFIID, void **);
detail::material_method_hooks<MakeLibrary, 44> original_library;
std::atomic<LoadStream> original_library_stream{};
HRESULT STDMETHODCALLTYPE load_stream(ID3D12PipelineLibrary1 *library, LPCWSTR name,
                                      const D3D12_PIPELINE_STATE_STREAM_DESC *desc, REFIID iid, void **output)
{
    const auto hr = original_library_stream.load()(library, name, desc, iid, output);
    if (SUCCEEDED(hr) && output && *output)
    {
        static std::atomic<bool> said{};
        if (!said.exchange(true))
            say("Material capture: observing cached library pipelines.");
        ComPtr<ID3D12Device2> device;
        if (SUCCEEDED(library->GetDevice(IID_PPV_ARGS(&device))))
            (void)observe_stream(device.Get(), desc, *output);
    }
    return hr;
}
HRESULT STDMETHODCALLTYPE make_library(ID3D12Device1 *device, const void *bytes, SIZE_T size, REFIID iid, void **output)
{
    const auto hr = original_library.load()(device, bytes, size, iid, output);
    if (SUCCEEDED(hr) && output && *output)
        try
        {
            ComPtr<ID3D12PipelineLibrary1> library;
            if (SUCCEEDED(static_cast<IUnknown *>(*output)->QueryInterface(IID_PPV_ARGS(&library))))
            {
                auto *table = *reinterpret_cast<void ***>(library.Get());
                void *relay{};
                const auto prepared = hook_prepare(table[13], reinterpret_cast<void *>(&load_stream), &relay);
                if (prepared == HookOk)
                {
                    original_library_stream.store(reinterpret_cast<LoadStream>(relay));
                    if (hook_enable(table[13]) == HookOk)
                        say("Material capture: native pipeline library observed.");
                }
            }
        }
        catch (...)
        {
        }
    return hr;
}
void STDMETHODCALLTYPE set(ID3D12GraphicsCommandList *l, ID3D12PipelineState *p)
{
    original_set.load()(l, p);
    if (private_commands)
        return;
    static std::atomic<bool> seen{};
    if (!seen.exchange(true))
        say("Material capture: observing command-list pipeline bindings.");
    try
    {
        auto &s = state();
        std::lock_guard lock(s.mutex);
        if (material_transmission_requested())
        {
            tracked_list(s, l).pipeline = p;
            if (find_pipeline(s, p) != s.pipelines.end())
            {
                static std::atomic<bool> said{};
                if (!said.exchange(true))
                    say("Material capture: matched native pipeline bound for drawing.");
            }
        }
    }
    catch (...)
    {
    }
}
void STDMETHODCALLTYPE clear_state(ID3D12GraphicsCommandList *l, ID3D12PipelineState *p)
{
    original_clear.load()(l, p);
    try
    {
        auto &s = state();
        std::lock_guard lock(s.mutex);
        if (material_transmission_requested())
        {
            auto &row = tracked_list(s, l);
            row = {};
            row.pipeline = p;
        }
    }
    catch (...)
    {
    }
}
HRESULT STDMETHODCALLTYPE reset(ID3D12GraphicsCommandList *l, ID3D12CommandAllocator *a, ID3D12PipelineState *p)
{
    auto hr = original_reset.load()(l, a, p);
    if (private_commands)
        return hr;
    if (SUCCEEDED(hr))
        material_list_reset(l);
    if (SUCCEEDED(hr))
        try
        {
            auto &s = state();
            std::lock_guard lock(s.mutex);
            if (material_transmission_requested())
            {
                auto &row = tracked_list(s, l);
                row = {};
                row.pipeline = p;
            }
        }
        catch (...)
        {
        }
    return hr;
}
void STDMETHODCALLTYPE targets(ID3D12GraphicsCommandList *l, UINT n, const D3D12_CPU_DESCRIPTOR_HANDLE *r,
                               BOOL contiguous, const D3D12_CPU_DESCRIPTOR_HANDLE *z)
{
    original_targets.load()(l, n, r, contiguous, z);
    if (private_commands)
        return;
    try
    {
        auto &s = state();
        std::lock_guard lock(s.mutex);
        if ((!material_transmission_requested()) || n > 8)
            return;
        auto &row = tracked_list(s, l);
        row.count = n;
        row.has_depth = z != nullptr;
        row.depth = z ? *z : D3D12_CPU_DESCRIPTOR_HANDLE{};
        const auto stride = s.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        for (UINT i = 0; i < n; ++i)
        {
            row.targets[i] = r[contiguous ? 0 : i];
            if (contiguous)
                row.targets[i].ptr += static_cast<SIZE_T>(i) * stride;
        }
    }
    catch (...)
    {
    }
}
void STDMETHODCALLTYPE viewports(ID3D12GraphicsCommandList *l, UINT n, const D3D12_VIEWPORT *v)
{
    original_viewports.load()(l, n, v);
    if (private_commands)
        return;
    try
    {
        auto &s = state();
        std::lock_guard lock(s.mutex);
        if ((material_transmission_requested()) && n == 1)
            tracked_list(s, l).viewport = v[0];
    }
    catch (...)
    {
    }
}
void capture_replay(ID3D12GraphicsCommandList *l, const std::function<void()> &replay)
{
    if (private_commands)
        return;
    try
    {
        auto &s = state();
        std::lock_guard lock(s.mutex);
        if (s.failed || (!material_transmission_requested()) || l->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT)
            return;
        const auto row = s.lists.find(l);
        if (row == s.lists.end())
            return;
        const auto found = find_pipeline(s, row->second.pipeline);
        if (found == s.pipelines.end())
            return;
        const auto &r = row->second;
        const auto &p = found->second;
        static std::atomic<bool> said{};
        if (!said.exchange(true))
            logging::printf(logging::Level::info, logging::Channel::graphics,
                            "Material capture: matching draw has %u targets; "
                            "viewport %.0fx%.0f at %.0f,%.0f.",
                            r.count, r.viewport.Width, r.viewport.Height, r.viewport.TopLeftX, r.viewport.TopLeftY);
        if (r.count != p.native_targets || r.viewport.TopLeftX != 0 || r.viewport.TopLeftY != 0 ||
            r.viewport.Width < 64 || r.viewport.Height < 64 || r.viewport.Width > 4096 || r.viewport.Height > 2160)
            return;
        if (material_transmission_configured())
        {
            if (!material_transmission_requested())
                return;
            MaterialMaskDraw draw;
            ComPtr<ID3D12Device> device;
            if (FAILED(p.mask->GetDevice(IID_PPV_ARGS(&device))))
                return;
            draw.device = device.Get();
            draw.commands = l;
            draw.target_count = p.desc.NumRenderTargets;
            std::copy_n(p.desc.RTVFormats, 8, draw.formats.begin());
            draw.viewport = r.viewport;
            draw.depth = r.has_depth ? r.depth : D3D12_CPU_DESCRIPTOR_HANDLE{};
            draw.source_colour = r.count ? r.targets[0] : D3D12_CPU_DESCRIPTOR_HANDLE{};
            draw.parameters = p.parameters;
            draw.replay = [&](D3D12_CPU_DESCRIPTOR_HANDLE handle) {
                struct Restore
                {
                    ID3D12GraphicsCommandList *list;
                    const ListState &row;
                    ~Restore()
                    {
                        original_set.load(list)(list, row.pipeline);
                        original_targets.load(list)(list, row.count, row.targets.data(), FALSE,
                                                    row.has_depth ? &row.depth : nullptr);
                    }
                } restore{l, r};
                original_targets.load(l)(l, p.desc.NumRenderTargets, &handle, TRUE, r.has_depth ? &r.depth : nullptr);
                original_set.load(l)(l, p.mask.Get());
                replay();
            };
            InternalScope internal(private_commands);
            record_material_mask(draw);
            return;
        }
    }
    catch (...)
    {
        state().failed = true;
        say("Material capture: stopped after an exception.");
    }
}
void STDMETHODCALLTYPE draw(ID3D12GraphicsCommandList *l, UINT indices, UINT instances, UINT first, INT base,
                            UINT instance)
{
    original_draw.load()(l, indices, instances, first, base, instance);
    static std::atomic<bool> seen{};
    if (!seen.exchange(true))
        say("Material capture: observing indexed draw submissions.");
    capture_replay(l, [&] { original_draw.load()(l, indices, instances, first, base, instance); });
}
void STDMETHODCALLTYPE draw_plain(ID3D12GraphicsCommandList *l, UINT vertices, UINT instances, UINT first,
                                  UINT instance)
{
    original_plain.load()(l, vertices, instances, first, instance);
    capture_replay(l, [&] { original_plain.load()(l, vertices, instances, first, instance); });
}
void STDMETHODCALLTYPE indirect(ID3D12GraphicsCommandList *l, ID3D12CommandSignature *signature, UINT maximum,
                                ID3D12Resource *arguments, UINT64 offset, ID3D12Resource *count, UINT64 count_offset)
{
    original_indirect.load()(l, signature, maximum, arguments, offset, count, count_offset);
    static std::atomic<bool> seen{};
    if (!seen.exchange(true))
        say("Material capture: observing indirect draw submissions.");
    capture_replay(l, [&] { original_indirect.load()(l, signature, maximum, arguments, offset, count, count_offset); });
}
void observe_submitted_lists(UINT, ID3D12CommandList *const *) noexcept;
void STDMETHODCALLTYPE execute(ID3D12CommandQueue *q, UINT n, ID3D12CommandList *const *lists)
{
    original_execute.load()(q, n, lists);
    if (private_commands)
        return;
    observe_submitted_lists(n, lists);
    material_queue_submission(q, n, lists);
}
} // namespace
namespace
{
void prepare_commands(ID3D12GraphicsCommandList *, ID3D12CommandQueue *) noexcept;
void observe_command_interfaces(ID3D12GraphicsCommandList *) noexcept;
using MakeQueue = HRESULT(STDMETHODCALLTYPE *)(ID3D12Device *, const D3D12_COMMAND_QUEUE_DESC *, REFIID, void **);
using MakeList = HRESULT(STDMETHODCALLTYPE *)(ID3D12Device *, UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator *,
                                              ID3D12PipelineState *, REFIID, void **);
using MakeList1 = HRESULT(STDMETHODCALLTYPE *)(ID3D12Device4 *, UINT, D3D12_COMMAND_LIST_TYPE, D3D12_COMMAND_LIST_FLAGS,
                                               REFIID, void **);
detail::material_method_hooks<MakeQueue, 8> original_queue;
detail::material_method_hooks<MakeList, 12> original_list;
detail::material_method_hooks<MakeList1, 51> original_list1;
HRESULT STDMETHODCALLTYPE make_queue(ID3D12Device *d, const D3D12_COMMAND_QUEUE_DESC *desc, REFIID iid, void **out)
{
    const auto hr = original_queue.load()(d, desc, iid, out);
    if (private_commands)
        return hr;
    if (SUCCEEDED(hr) && out && *out)
    {
        ComPtr<ID3D12CommandQueue> queue;
        if (SUCCEEDED(static_cast<IUnknown *>(*out)->QueryInterface(IID_PPV_ARGS(&queue))))
            prepare_commands(nullptr, queue.Get());
    }
    return hr;
}
HRESULT STDMETHODCALLTYPE make_list(ID3D12Device *d, UINT node, D3D12_COMMAND_LIST_TYPE type,
                                    ID3D12CommandAllocator *allocator, ID3D12PipelineState *pipeline, REFIID iid,
                                    void **out)
{
    const auto hr = original_list.load()(d, node, type, allocator, pipeline, iid, out);
    if (private_commands)
        return hr;
    if (SUCCEEDED(hr) && out && *out &&
        (type == D3D12_COMMAND_LIST_TYPE_DIRECT || type == D3D12_COMMAND_LIST_TYPE_BUNDLE))
    {
        ComPtr<ID3D12GraphicsCommandList> list;
        if (SUCCEEDED(static_cast<IUnknown *>(*out)->QueryInterface(IID_PPV_ARGS(&list))))
        {
            observe_command_interfaces(list.Get());
            auto &s = state();
            std::lock_guard lock(s.mutex);
            tracked_list(s, list.Get()).pipeline = pipeline;
        }
    }
    return hr;
}
HRESULT STDMETHODCALLTYPE make_list1(ID3D12Device4 *d, UINT node, D3D12_COMMAND_LIST_TYPE type,
                                     D3D12_COMMAND_LIST_FLAGS flags, REFIID iid, void **out)
{
    const auto hr = original_list1.load()(d, node, type, flags, iid, out);
    if (private_commands)
        return hr;
    if (SUCCEEDED(hr) && out && *out &&
        (type == D3D12_COMMAND_LIST_TYPE_DIRECT || type == D3D12_COMMAND_LIST_TYPE_BUNDLE))
    {
        ComPtr<ID3D12GraphicsCommandList> list;
        if (SUCCEEDED(static_cast<IUnknown *>(*out)->QueryInterface(IID_PPV_ARGS(&list))))
        {
            observe_command_interfaces(list.Get());
        }
    }
    return hr;
}
using MakeDsv = void(STDMETHODCALLTYPE *)(ID3D12Device *, ID3D12Resource *, const D3D12_DEPTH_STENCIL_VIEW_DESC *,
                                          D3D12_CPU_DESCRIPTOR_HANDLE);
using CopySimple = void(STDMETHODCALLTYPE *)(ID3D12Device *, UINT, D3D12_CPU_DESCRIPTOR_HANDLE,
                                             D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_DESCRIPTOR_HEAP_TYPE);
using CopyRanges = void(STDMETHODCALLTYPE *)(ID3D12Device *, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE *, const UINT *,
                                             UINT, const D3D12_CPU_DESCRIPTOR_HANDLE *, const UINT *,
                                             D3D12_DESCRIPTOR_HEAP_TYPE);
detail::material_method_hooks<MakeDsv, 21> original_dsv;
detail::material_method_hooks<CopySimple, 24> original_copy_simple;
detail::material_method_hooks<CopyRanges, 23> original_copy_ranges;
void STDMETHODCALLTYPE make_dsv(ID3D12Device *d, ID3D12Resource *resource, const D3D12_DEPTH_STENCIL_VIEW_DESC *desc,
                                D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
    original_dsv.load()(d, resource, desc, handle);
    if (!private_commands)
        material_depth_descriptor(resource, desc, handle);
}
void STDMETHODCALLTYPE copy_simple(ID3D12Device *d, UINT count, D3D12_CPU_DESCRIPTOR_HANDLE destination,
                                   D3D12_CPU_DESCRIPTOR_HANDLE source, D3D12_DESCRIPTOR_HEAP_TYPE type)
{
    original_copy_simple.load()(d, count, destination, source, type);
    if (!private_commands && type == D3D12_DESCRIPTOR_HEAP_TYPE_DSV)
        material_copy_depth_descriptors(d, count, destination, source);
}
void STDMETHODCALLTYPE copy_ranges(ID3D12Device *d, UINT nd, const D3D12_CPU_DESCRIPTOR_HANDLE *dest, const UINT *dc,
                                   UINT ns, const D3D12_CPU_DESCRIPTOR_HANDLE *src, const UINT *sc,
                                   D3D12_DESCRIPTOR_HEAP_TYPE type)
{
    original_copy_ranges.load()(d, nd, dest, dc, ns, src, sc, type);
    if (private_commands || type != D3D12_DESCRIPTOR_HEAP_TYPE_DSV || nd > 8192 || ns > 8192)
        return;
    const auto stride = d->GetDescriptorHandleIncrementSize(type);
    UINT di{}, si{}, dx{}, sx{};
    while (di < nd && si < ns)
    {
        const auto dn = dc ? dc[di] : 1, sn = sc ? sc[si] : 1;
        if (dx == dn)
        {
            ++di;
            dx = 0;
            continue;
        }
        if (sx == sn)
        {
            ++si;
            sx = 0;
            continue;
        }
        const auto count = (std::min)(dn - dx, sn - sx);
        auto a = dest[di], b = src[si];
        a.ptr += SIZE_T{dx} * stride;
        b.ptr += SIZE_T{sx} * stride;
        material_copy_depth_descriptors(d, count, a, b);
        dx += count;
        sx += count;
    }
}
void prepare_device(ID3D12Device *device) noexcept
{
    static std::atomic<bool> installed{};
    if (configured_recipes().empty() || !device || installed.exchange(true))
        return;
    try
    {
        auto &s = state();
        s.device = device;
        s.recipes = configured_recipes();
        set_material_transmission_enabled(true);
        bool ok = true;
        HookBatch batch;
        const auto install = [&](void *target, void *replacement, auto publish) {
            void *relay{};
            if (hook_prepare(target, replacement, &relay) != HookOk)
                return false;
            publish(relay);
            return hook_enable(target) == HookOk;
        };
        const auto observe = [&]<class Interface>(Interface *version) {
            ok = original_queue.install(version, &make_queue, install) && ok;
            ok = original_list.install(version, &make_list, install) && ok;
            ok = original_make.install(version, &make, install) && ok;
            ok = original_dsv.install(version, &make_dsv, install) && ok;
            ok = original_copy_simple.install(version, &copy_simple, install) && ok;
            ok = original_copy_ranges.install(version, &copy_ranges, install) && ok;
            if constexpr (std::is_base_of_v<ID3D12Device1, Interface>)
                ok = original_library.install(version, &make_library, install) && ok;
            if constexpr (std::is_base_of_v<ID3D12Device2, Interface>)
                ok = original_stream.install(version, &make_stream, install) && ok;
            if constexpr (std::is_base_of_v<ID3D12Device4, Interface>)
                ok = original_list1.install(version, &make_list1, install) && ok;
        };
        observe(device);
        const auto query = [&]<class Interface>() {
            ComPtr<Interface> version;
            if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&version))))
                observe(version.Get());
        };
        query.operator()<ID3D12Device1>();
        query.operator()<ID3D12Device2>();
        query.operator()<ID3D12Device3>();
        query.operator()<ID3D12Device4>();
        query.operator()<ID3D12Device5>();
        query.operator()<ID3D12Device6>();
        query.operator()<ID3D12Device7>();
        query.operator()<ID3D12Device8>();
        query.operator()<ID3D12Device9>();
        query.operator()<ID3D12Device10>();
        query.operator()<ID3D12Device11>();
        query.operator()<ID3D12Device12>();
        query.operator()<ID3D12Device13>();
        query.operator()<ID3D12Device14>();
        s.failed = !ok;
        say(ok ? "Material capture: observing device pipelines before scene "
                 "loading."
               : "Material capture: early device hook failed; probe disabled.");
    }
    catch (...)
    {
        state().failed = true;
        say("Material capture: early setup failed.");
    }
}
using CreateDevice = HRESULT(WINAPI *)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
std::atomic<CreateDevice> original_device{};
HRESULT WINAPI create_device(IUnknown *adapter, D3D_FEATURE_LEVEL level, REFIID iid, void **output)
{
    const auto result = original_device.load()(adapter, level, iid, output);
    if (SUCCEEDED(result) && output && *output)
    {
        ComPtr<ID3D12Device> device;
        if (SUCCEEDED(static_cast<IUnknown *>(*output)->QueryInterface(IID_PPV_ARGS(&device))))
            prepare_device(device.Get());
    }
    return result;
}
} // namespace
void start_material_capture() noexcept
{
    if (configured_recipes().empty())
        return;
    auto *target = reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "D3D12CreateDevice"));
    void *relay{};
    if (!target || hook_prepare(target, reinterpret_cast<void *>(&create_device), &relay) != HookOk)
        return;
    original_device.store(reinterpret_cast<CreateDevice>(relay));
    if (hook_enable(target) == HookOk)
        say("Material capture: waiting for native device creation.");
}
namespace
{
void prepare_commands(ID3D12GraphicsCommandList *list, ID3D12CommandQueue *queue) noexcept
{
    try
    {
        auto &s = state();
        if (s.failed || s.recipes.empty())
            return;
        bool ok = true;
        HookBatch batch;
        const auto install = [&](void *target, void *replacement, auto publish) {
            void *relay{};
            const auto prepared = hook_prepare(target, replacement, &relay);
            if (prepared != HookOk)
            {
                logging::log(logging::Level::warning, logging::Channel::graphics,
                             "Material transmission: hook prepare failed: {}", hook_status_string(prepared));
                return false;
            }
            publish(relay);
            const auto enabled = hook_enable(target);
            if (enabled != HookOk)
                logging::log(logging::Level::warning, logging::Channel::graphics,
                             "Material transmission: hook enable failed: {}", hook_status_string(enabled));
            return enabled == HookOk;
        };
        if (list)
        {
            detail::install_depth_probe(list);
            ok = original_set.install(list, &set, install) && ok;
            ok = original_clear.install(list, &clear_state, install) && ok;
            ok = original_plain.install(list, &draw_plain, install) && ok;
            ok = original_reset.install(list, &reset, install) && ok;
            ok = original_targets.install(list, &targets, install) && ok;
            ok = original_viewports.install(list, &viewports, install) && ok;
            ok = original_draw.install(list, &draw, install) && ok;
            ok = original_indirect.install(list, &indirect, install) && ok;
        }

        if (queue)
            ok = original_execute.install(queue, &execute, install) && ok;
        if (!ok)
        {
            s.failed = true;
            say("Material capture: command hook setup failed; probe disabled.");
        }
    }
    catch (...)
    {
        state().failed = true;
        say("Material capture: command setup failed; probe disabled.");
    }
}
// Different COM interface versions can have different forwarding thunks even on
// one command-list object. Observing only QueryInterface(base) misses those
// calls.
void observe_command_interfaces(ID3D12GraphicsCommandList *list) noexcept
{
    if (!list)
        return;
    prepare_commands(list, nullptr);
    const auto observe = [&]<class Interface>() {
        Microsoft::WRL::ComPtr<Interface> version;
        if (SUCCEEDED(list->QueryInterface(IID_PPV_ARGS(&version))))
            prepare_commands(version.Get(), nullptr);
    };
    observe.operator()<ID3D12GraphicsCommandList1>();
    observe.operator()<ID3D12GraphicsCommandList2>();
    observe.operator()<ID3D12GraphicsCommandList3>();
    observe.operator()<ID3D12GraphicsCommandList4>();
    observe.operator()<ID3D12GraphicsCommandList5>();
    observe.operator()<ID3D12GraphicsCommandList6>();
    observe.operator()<ID3D12GraphicsCommandList7>();
    observe.operator()<ID3D12GraphicsCommandList8>();
    observe.operator()<ID3D12GraphicsCommandList9>();
    observe.operator()<ID3D12GraphicsCommandList10>();
}
void observe_submitted_lists(UINT count, ID3D12CommandList *const *lists) noexcept
{
    try
    {
        static std::mutex mutex;
        static std::map<void *, bool> implementations;
        for (UINT i = 0; i < count; ++i)
        {
            if (!lists[i])
                continue;
            ComPtr<ID3D12GraphicsCommandList> graphics;
            if (FAILED(lists[i]->QueryInterface(IID_PPV_ARGS(&graphics))))
                continue;
            const auto install = [&](ID3D12GraphicsCommandList *list) {
                if (!list)
                    return;
                auto *table = *reinterpret_cast<void ***>(list);
                bool fresh{};
                {
                    std::lock_guard lock(mutex);
                    if (implementations.size() < 128)
                        fresh = implementations.emplace(table, true).second;
                }
                if (fresh)
                {
                    observe_command_interfaces(list);
                    say("Material capture: observed submitted command-list "
                        "implementation.");
                }
            };
            install(graphics.Get());
            constexpr GUID base{0xadec44e2, 0x61f0, 0x45c3, {0xad, 0x9f, 0x1b, 0x37, 0x37, 0x92, 0x84, 0xff}};
            ComPtr<IUnknown> native;
            if (SUCCEEDED(graphics->QueryInterface(base, reinterpret_cast<void **>(native.GetAddressOf()))))
            {
                ComPtr<ID3D12GraphicsCommandList> underlying;
                if (SUCCEEDED(native.As(&underlying)))
                    install(underlying.Get());
            }
        }
    }
    catch (...)
    {
        say("Material capture: submitted implementation inspection failed.");
    }
}
} // namespace
void install_material_capture(ID3D12Device *device, ID3D12GraphicsCommandList *list, ID3D12CommandQueue *queue) noexcept
{
    if (configured_recipes().empty() || !device)
        return;
    prepare_device(device);
    observe_command_interfaces(list);
    prepare_commands(nullptr, queue);
}
void observe_material_depth_barriers(ID3D12GraphicsCommandList *list, UINT count,
                                     const D3D12_RESOURCE_BARRIER *barriers, MaterialBarrierFn original) noexcept
{
    if (private_commands)
        return;
    InternalScope internal(private_commands);
    material_depth_barriers(list, count, barriers, original);
}
void present_material_transmission_guarded(ID3D12Device *device, ID3D12CommandQueue *queue, ID3D12Resource *buffer,
                                           int transfer) noexcept
{
    InternalScope internal(private_commands);
    present_material_transmission(device, queue, buffer, transfer);
}
} // namespace dingosdk::overlay
