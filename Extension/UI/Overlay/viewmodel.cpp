#include "viewmodel.h"
#include "Extension/Plugins/plugin_camera.h"
#include "viewmodel_hlsl.h" // generated from viewmodel.hlsl (cmake/UI.cmake)
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/clock.h"
#include <Windows.h>
#include <d3dcompiler.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <numbers>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace dingosdk::viewmodel {
namespace {
using Microsoft::WRL::ComPtr;

constexpr std::uintmax_t most_file_bytes = 64ull * 1024 * 1024, most_picture_bytes = 64ull * 1024 * 1024;
constexpr UINT most_picture_side = 2048; // larger pictures are scaled down to this as they are read
constexpr float near_plane = 0.01f, far_plane = 5000.0f;
// The frame's constants (viewmodel.hlsl's cbuffer frame): nine float4s, then the bones' rows.
constexpr std::size_t frame_header_floats = 29 * 4, frame_floats = frame_header_floats + max_bones * 12;
constexpr std::size_t constants_stride = (frame_floats * 4 + 255) / 256 * 256; // root CBVs are 256-byte aligned
constexpr std::size_t constants_ring = 16;


// A picture as the GPU takes it: RGBA8, its mip levels one after another, largest first.
struct SceneDepth {
    std::mutex mutex;
    ComPtr<ID3D12Resource> texture;
    UINT64 width{};
    UINT height{};
    std::atomic<bool> wanted{};
    std::atomic<double> deadline{};
    bool copied{}, valid{};
    DXGI_FORMAT format{}, wanted_format{DXGI_FORMAT_R32G8X24_TYPELESS};
};
SceneDepth& scene_depth() { static auto* s = new SceneDepth; return *s; }

struct Picture {
    std::vector<std::uint8_t> rgba;
    UINT width{}, height{}, levels{};
    bool srgb{};
};
// What the background reading made: the model and its pictures, three a material (colour, normal,
// surface), each possibly empty.
struct Loaded {
    Model model;
    std::vector<Picture> pictures;
};

struct ReadRequest {
    std::filesystem::path file;
    std::vector<std::filesystem::path> attachments;
    std::uint64_t serial{};
};
struct Shared {
    std::mutex mutex;
    std::uint64_t serial{}; // which load is wanted; a reading that finishes for an older one is dropped
    Status status{Status::none};
    std::string why;
    std::shared_ptr<const Loaded> loaded;
    Look look;
    double until{};
    Animator animator;
    bool render_ready{};
    std::uint64_t prepared_serial{};
    Status prepared_status{Status::none};
    std::string prepared_why;
    std::shared_ptr<const Loaded> prepared;
    bool prepared_ready{},preparing{};
    std::optional<ReadRequest> next_read;
};
Shared& shared() {
    static auto* value = new Shared;
    return *value;
}
struct DisplayShared {
    bool reading{};
    bool pending{};
    std::mutex mutex;
    std::filesystem::path file;
    std::optional<Display> look;
    std::shared_ptr<const Loaded> loaded;
    std::uint64_t serial{};
    double until{};
    Status status{Status::none};
    std::string why;
};
DisplayShared& display_shared() {static auto* value=new DisplayShared;return *value;}

// ---- reading ---------------------------------------------------------------------------------------
bool read_file(const std::filesystem::path& file, std::uintmax_t most, std::string& out, std::string& why) {
    std::error_code code;
    const auto size = std::filesystem::file_size(file, code);
    if (code) {
        why = "\"" + file.filename().string() + "\" cannot be read";
        return false;
    }
    if (size > most) {
        why = "\"" + file.filename().string() + "\" is larger than " + std::to_string(most >> 20) + " MB";
        return false;
    }
    std::ifstream in(file, std::ios::binary);
    out.resize(static_cast<std::size_t>(size));
    if (!in.read(out.data(), static_cast<std::streamsize>(out.size()))) {
        why = "\"" + file.filename().string() + "\" cannot be read";
        return false;
    }
    return true;
}

// A picture's name from the model file: a plain file name beside it (the file is a stranger's, so it
// must not lead anywhere else).
bool plain_name(const std::string& name) {
    if (name.empty() || name.size() > 64 || name.front() == '.') return false;
    for (const unsigned char c : name)
        if (!(std::isalnum(c) || c == '_' || c == '-' || c == '.')) return false;
    auto lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower.ends_with(".png") || lower.ends_with(".jpg") || lower.ends_with(".jpeg");
}

// The picture's pixels, at most most_picture_side a side, as RGBA8.
bool decode(const std::string& bytes, Picture& out) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool ok{};
    {
        ComPtr<IWICImagingFactory> factory;
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> frame;
        ComPtr<IWICFormatConverter> converter;
        ComPtr<IWICBitmapScaler> scaler;
        UINT width{}, height{};
        if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
            SUCCEEDED(factory->CreateStream(&stream)) &&
            SUCCEEDED(stream->InitializeFromMemory(reinterpret_cast<BYTE*>(const_cast<char*>(bytes.data())), static_cast<DWORD>(bytes.size()))) &&
            SUCCEEDED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) &&
            SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(frame->GetSize(&width, &height)) && width && height && width <= 16384 &&
            height <= 16384 && SUCCEEDED(factory->CreateFormatConverter(&converter))) {
            IWICBitmapSource* source = frame.Get();
            if (std::max(width, height) > most_picture_side) {
                const double scale = static_cast<double>(most_picture_side) / std::max(width, height);
                width = std::max(1u, static_cast<UINT>(width * scale));
                height = std::max(1u, static_cast<UINT>(height * scale));
                if (FAILED(factory->CreateBitmapScaler(&scaler)) || FAILED(scaler->Initialize(frame.Get(), width, height, WICBitmapInterpolationModeFant)))
                    source = nullptr;
                else source = scaler.Get();
            }
            if (source && SUCCEEDED(converter->Initialize(source, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0,
                                                          WICBitmapPaletteTypeCustom))) {
                out.rgba.resize(static_cast<std::size_t>(width) * height * 4);
                out.width = width;
                out.height = height;
                ok = SUCCEEDED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(out.rgba.size()), out.rgba.data()));
            }
        }
    }
    if (SUCCEEDED(com)) CoUninitialize();
    return ok;
}

// Each smaller level the average of four of the one above (an odd edge repeats its last pixel).
void make_levels(Picture& picture) {
    UINT width = picture.width, height = picture.height;
    std::size_t at = 0;
    picture.levels = 1;
    while (width > 1 || height > 1) {
        const UINT w = std::max(1u, width / 2), h = std::max(1u, height / 2);
        const std::size_t next = at + std::size_t{width} * height * 4;
        picture.rgba.resize(next + std::size_t{w} * h * 4);
        for (UINT y = 0; y < h; ++y)
            for (UINT x = 0; x < w; ++x)
                for (int c = 0; c < 4; ++c) {
                    const UINT x0 = std::min(x * 2, width - 1), x1 = std::min(x * 2 + 1, width - 1);
                    const UINT y0 = std::min(y * 2, height - 1), y1 = std::min(y * 2 + 1, height - 1);
                    const auto px = [&](UINT px_x, UINT px_y) { return picture.rgba[at + (std::size_t{px_y} * width + px_x) * 4 + c]; };
                    picture.rgba[next + (std::size_t{y} * w + x) * 4 + c] =
                        static_cast<std::uint8_t>((px(x0, y0) + px(x1, y0) + px(x0, y1) + px(x1, y1) + 2) / 4);
                }
        at = next;
        width = w;
        height = h;
        ++picture.levels;
    }
}

bool read_one(const std::filesystem::path& file, Loaded& loaded, std::size_t& image_bytes, std::string& why) {
    std::string bytes;
    if (!read_file(file,most_file_bytes,bytes,why)) return false;
    auto model=parse(bytes,why);if (!model) return false;
    loaded.model=std::move(*model);loaded.pictures.resize(loaded.model.materials.size()*3);
    for (std::size_t m=0;m<loaded.model.materials.size();++m) {
        const auto& material=loaded.model.materials[m];
        const std::array<const std::string*,3> names{&material.albedo,&material.normal,&material.surface};
        for (std::size_t k=0;k<3;++k) {
            if (names[k]->empty()) continue;
            if (!plain_name(*names[k])) {why="a material picture must be beside its model";return false;}
            std::string data;
            if (!read_file(file.parent_path()/ *names[k],most_picture_bytes,data,why)) return false;
            auto& picture=loaded.pictures[m*3+k];
            if (!decode(data,picture)) {why="a model picture cannot be decoded";return false;}
            picture.srgb=k==0 || (material.faces&64);make_levels(picture);image_bytes+=picture.rgba.size();
            if (image_bytes>(512ull<<20)) {why="combined model pictures exceed 512 MB";return false;}
        }
    }
    return true;
}
void read_in_background(std::filesystem::path file,std::vector<std::filesystem::path> attachments,std::uint64_t serial,bool preparing) {
    auto loaded=std::make_shared<Loaded>();std::string why;bool ok=false;
    try {
        std::size_t image_bytes = 0;
        ok = read_one(file, *loaded, image_bytes, why);
        for (const auto& path : attachments) {
            if (!ok) break;
            {
                auto& state = shared();
                std::lock_guard lock(state.mutex);
                if ((preparing ? state.prepared_serial : state.serial) != serial) return;
            }
            Loaded addition;
            ok = read_one(path, addition, image_bytes, why);
            if (!ok) break;
            ok=append_model(loaded->model,std::move(addition.model),why);
            if (ok) loaded->pictures.insert(loaded->pictures.end(),std::make_move_iterator(addition.pictures.begin()),std::make_move_iterator(addition.pictures.end()));
        }
    } catch (const std::exception&) {why="model bundle could not be read within its resource limits";ok=false;}
    auto& s=shared();std::lock_guard lock(s.mutex);
    if (preparing) {
        if (s.prepared_serial!=serial) return;
        s.prepared_ready=false;
        s.prepared_status=ok?Status::ready:Status::failed;s.prepared_why=why;
        s.prepared=ok?std::move(loaded):nullptr;
        return;
    }
    if (s.serial!=serial) return;
    s.render_ready=false;
    if (!ok) {s.status=Status::failed;s.why=why;logging::write(logging::Level::warning,logging::Channel::graphics,"Viewmodel "+file.filename().string()+" not loaded: "+why);return;}
    s.status=Status::ready;s.why.clear();s.loaded=std::move(loaded);s.animator.stop();
}

void prepare_worker() {
    for (;;) {
        ReadRequest request;
        {
            auto& s=shared();std::lock_guard lock(s.mutex);
            if (!s.next_read) {s.preparing=false;return;}
            request=std::move(*s.next_read);s.next_read.reset();
        }
        read_in_background(std::move(request.file),std::move(request.attachments),request.serial,true);
    }
}

// ---- drawing -----------------------------------------------------------------------------------------
struct Retired {
    ComPtr<ID3D12Resource> resource;
    std::uint64_t fence{};
};
struct Gpu {
    std::array<ComPtr<ID3D12Resource>,2> planar;
    ComPtr<ID3D12DescriptorHeap> planar_rtvs;
    ComPtr<ID3D12Resource> capture_depth;
    ComPtr<ID3D12DescriptorHeap> capture_depth_view;
    UINT64 capture_width{};
    UINT capture_height{};
    std::array<ComPtr<ID3D12PipelineState>,6> capture_pipelines;
    ComPtr<ID3D12PipelineState> capture_background;
    DXGI_FORMAT capture_format{DXGI_FORMAT_UNKNOWN};
    ComPtr<ID3D12PipelineState> inset_background;
    ComPtr<ID3D12RootSignature> root;
    std::array<ComPtr<ID3D12PipelineState>, 6> pipelines;
    DXGI_FORMAT pipeline_format{DXGI_FORMAT_UNKNOWN};
    std::shared_ptr<const Loaded> model; // what the geometry and textures below are of
    ComPtr<ID3D12Resource> vertices, indices;
    D3D12_VERTEX_BUFFER_VIEW vertex_view{};
    D3D12_INDEX_BUFFER_VIEW index_view{};
    std::vector<ComPtr<ID3D12Resource>> textures; // three a material, as Loaded::pictures
    std::size_t next_texture{};
    ID3D12DescriptorHeap* view_heap{};
    unsigned view_slot{~0u};
    std::size_t views_made{};  // textures whose views are in the heap
    ComPtr<ID3D12Resource> depth;
    ComPtr<ID3D12DescriptorHeap> depth_view;
    UINT64 depth_width{};
    UINT depth_height{};
    ComPtr<ID3D12Resource> constants;
    std::uint8_t* constants_mapped{};
    std::array<std::uint64_t, constants_ring> constants_fence{};
    std::size_t constants_next{};
    std::vector<Retired> retired;
    bool failed{};
    std::uint64_t upload_fence{};
};
thread_local Gpu* selected_gpu{};
Gpu& gpu() {
    if (selected_gpu) return *selected_gpu;
    static auto* value = new Gpu;
    return *value;
}
Gpu& prepared_gpu() {static auto* value=new Gpu;return *value;}
Gpu& display_gpu() {static auto* value=new Gpu;return *value;}
struct Surface {
    ComPtr<ID3D12Resource> colour;
    ComPtr<ID3D12DescriptorHeap> rtv;
};
Surface& surface() {static auto* value=new Surface;return *value;}
struct GpuScope {
    Gpu* before=selected_gpu;
    explicit GpuScope(Gpu& value) {selected_gpu=&value;}
    ~GpuScope() {selected_gpu=before;}
};
struct CaptureScope {
    Gpu& g;
    explicit CaptureScope(Gpu& value):g(value) {swap();}
    ~CaptureScope() {swap();}
    void swap() noexcept {
        std::swap(g.depth,g.capture_depth);std::swap(g.depth_view,g.capture_depth_view);
        std::swap(g.depth_width,g.capture_width);std::swap(g.depth_height,g.capture_height);
        std::swap(g.pipelines,g.capture_pipelines);std::swap(g.inset_background,g.capture_background);std::swap(g.pipeline_format,g.capture_format);
    }
};

void say_failed(const std::string& why) {
    auto& g = gpu();
    if (g.failed) return;
    g.failed = true;
    logging::write(logging::Level::warning, logging::Channel::graphics, "Viewmodels unavailable: " + why + " Plugins showing one get nothing drawn.");
}

void transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    if (from == to) return;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = from;
    barrier.Transition.StateAfter = to;
    list->ResourceBarrier(1, &barrier);
}

void retire(ComPtr<ID3D12Resource>& resource, std::uint64_t fence) {
    if (resource) gpu().retired.push_back({std::move(resource), fence});
    resource.Reset();
}

ComPtr<ID3D12Resource> buffer_in_upload_heap(ID3D12Device* device, std::size_t size, const wchar_t* name) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = size;
    description.Height = 1;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> made;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&made))))
        return {};
    made->SetName(name);
    return made;
}

bool fill(ID3D12Resource* buffer, const void* data, std::size_t size) {
    void* mapped{};
    const D3D12_RANGE none{};
    if (FAILED(buffer->Map(0, &none, &mapped)) || !mapped) return false;
    std::memcpy(mapped, data, size);
    buffer->Unmap(0, nullptr);
    return true;
}

bool build_root(ID3D12Device* device) {
    auto& g = gpu();
    if (g.root) return true;
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 3;
    D3D12_DESCRIPTOR_RANGE scene_range{};
    scene_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    scene_range.BaseShaderRegister = 3;
    scene_range.NumDescriptors = 1;
    D3D12_DESCRIPTOR_RANGE planar_range{};planar_range.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    planar_range.BaseShaderRegister=4;planar_range.NumDescriptors=2;
    std::array<D3D12_ROOT_PARAMETER, 6> parameters{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants.ShaderRegister = 0;
    parameters[0].Constants.Num32BitValues = 24;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[1].Descriptor.ShaderRegister = 1;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[2].DescriptorTable.NumDescriptorRanges = 1;
    parameters[2].DescriptorTable.pDescriptorRanges = &range;
    parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[3].DescriptorTable.NumDescriptorRanges = 1;
    parameters[3].DescriptorTable.pDescriptorRanges = &scene_range;
    parameters[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[4].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[4].Constants.ShaderRegister=2;parameters[4].Constants.Num32BitValues=12;
    parameters[4].ShaderVisibility=D3D12_SHADER_VISIBILITY_VERTEX;
    parameters[5].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[5].DescriptorTable.NumDescriptorRanges=1;parameters[5].DescriptorTable.pDescriptorRanges=&planar_range;
    parameters[5].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_ANISOTROPIC;
    sampler.MaxAnisotropy = 8;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    constexpr std::array address{D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_WRAP,
                                 D3D12_TEXTURE_ADDRESS_MODE_MIRROR};
    std::array<D3D12_STATIC_SAMPLER_DESC, 10> samplers{};
    samplers[0] = sampler;
    for (unsigned u = 0; u < address.size(); ++u) for (unsigned v = 0; v < address.size(); ++v) {
        auto& layer = samplers[1 + u * 3 + v];
        layer = sampler;
        layer.AddressU = address[u]; layer.AddressV = address[v];
        layer.ShaderRegister = 1 + u * 3 + v;
    }
    D3D12_ROOT_SIGNATURE_DESC description{};
    description.NumParameters = static_cast<UINT>(parameters.size());
    description.pParameters = parameters.data();
    description.NumStaticSamplers = static_cast<UINT>(samplers.size());
    description.pStaticSamplers = samplers.data();
    description.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT | D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
                        D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS | D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;
    ComPtr<ID3DBlob> blob, error;
    if (FAILED(D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)) || !blob) {
        say_failed("its root signature could not be built.");
        return false;
    }
    if (FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g.root)))) {
        say_failed("its root signature was refused.");
        return false;
    }
    return true;
}

bool build_pipeline(ID3D12Device* device, DXGI_FORMAT format) {
    auto& g = gpu();
    if (g.pipelines[0] && g.pipeline_format == format) return true;
    for (auto& pipeline : g.pipelines) pipeline.Reset();
    if (!build_root(device)) return false;
    ComPtr<ID3DBlob> vertex, pixel, error;
    const auto length = std::strlen(viewmodel_hlsl);
    if (FAILED(D3DCompile(viewmodel_hlsl, length, "viewmodel", nullptr, nullptr, "vs_main", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vertex, &error)) ||
        !vertex) {
        say_failed(std::string("its vertex shader did not compile") +
                   (error ? std::string(": ") + static_cast<const char*>(error->GetBufferPointer()) : std::string(".")));
        return false;
    }
    error.Reset();
    if (FAILED(D3DCompile(viewmodel_hlsl, length, "viewmodel", nullptr, nullptr, "ps_main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &pixel, &error)) ||
        !pixel) {
        say_failed(std::string("its pixel shader did not compile") +
                   (error ? std::string(": ") + static_cast<const char*>(error->GetBufferPointer()) : std::string(".")));
        return false;
    }
    const std::array<D3D12_INPUT_ELEMENT_DESC, 9> layout{{
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Vertex, position), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Vertex, normal), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TANGENT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(Vertex, tangent), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(Vertex, uv), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(Vertex, uv1), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 2, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(Vertex, uv2), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"BLENDINDICES", 0, DXGI_FORMAT_R8G8B8A8_UINT, 0, offsetof(Vertex, joints), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"BLENDWEIGHT", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, offsetof(Vertex, weights), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR",0,DXGI_FORMAT_R8G8B8A8_UNORM,0,offsetof(Vertex,colour),D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
    }};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
    description.pRootSignature = g.root.Get();
    description.VS = {vertex->GetBufferPointer(), vertex->GetBufferSize()};
    description.PS = {pixel->GetBufferPointer(), pixel->GetBufferSize()};
    description.InputLayout = {layout.data(), static_cast<UINT>(layout.size())};
    description.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE;
    description.SampleMask = UINT_MAX;
    description.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    // Both sides: the exporter's change of axes is a reflection, and a model's thin parts are seen from
    // either side; the depth buffer keeps the nearest.
    description.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    description.RasterizerState.DepthClipEnable = TRUE;
    description.DepthStencilState.DepthEnable = TRUE;
    description.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    // Reversed floating-point depth retains precision across large scenes.
    // Equal depth also honours the authored order of coincident overlays.
    description.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    description.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    description.NumRenderTargets = 1;
    description.RTVFormats[0] = format;
    description.SampleDesc.Count = 1;
    for (unsigned i = 0; i < g.pipelines.size(); ++i) {
        const bool transparent = i >= 3;
        description.RasterizerState.CullMode = i % 3 == 0 ? D3D12_CULL_MODE_NONE : i % 3 == 1 ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_FRONT;
        description.DepthStencilState.DepthWriteMask = transparent ? D3D12_DEPTH_WRITE_MASK_ZERO : D3D12_DEPTH_WRITE_MASK_ALL;
        auto& blend = description.BlendState.RenderTarget[0];
        blend.BlendEnable = transparent;
        blend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        blend.BlendOp = D3D12_BLEND_OP_ADD;
        blend.SrcBlendAlpha = D3D12_BLEND_ONE;
        blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        if (FAILED(device->CreateGraphicsPipelineState(&description, IID_PPV_ARGS(&g.pipelines[i])))) {
            say_failed("its pipeline was refused for this swapchain format.");
            return false;
        }
    }
    g.pipeline_format = format;
    ComPtr<ID3DBlob> inset_vertex,inset_pixel;
    if (FAILED(D3DCompile(viewmodel_hlsl,length,"viewmodel inset",nullptr,nullptr,"inset_vs","vs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&inset_vertex,&error)) ||
        FAILED(D3DCompile(viewmodel_hlsl,length,"viewmodel inset",nullptr,nullptr,"inset_ps","ps_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&inset_pixel,&error))) {
        say_failed("its inset shader could not compile.");return false;
    }
    description.VS={inset_vertex->GetBufferPointer(),inset_vertex->GetBufferSize()};
    description.PS={inset_pixel->GetBufferPointer(),inset_pixel->GetBufferSize()};
    description.InputLayout={};description.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
    description.DepthStencilState.DepthEnable=FALSE;description.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ZERO;
    if (FAILED(device->CreateGraphicsPipelineState(&description,IID_PPV_ARGS(&g.inset_background)))) {
        say_failed("its inset background pipeline was refused.");return false;
    }
    return true;
}

// The depth buffer, the size of the back buffer.
bool ensure_depth(const Target& t, UINT64 width, UINT height) {
    auto& g = gpu();
    if (g.depth && g.depth_width == width && g.depth_height == height) return true;
    retire(g.depth, t.fence);
    if (!g.depth_view) {
        D3D12_DESCRIPTOR_HEAP_DESC heap{};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        heap.NumDescriptors = 1;
        if (FAILED(t.device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&g.depth_view)))) {
            say_failed("its depth view could not be made.");
            return false;
        }
    }
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = width;
    description.Height = height;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.Format = DXGI_FORMAT_D32_FLOAT;
    description.SampleDesc.Count = 1;
    description.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_CLEAR_VALUE clear{};
    clear.Format = DXGI_FORMAT_D32_FLOAT;
    clear.DepthStencil.Depth = 0.0f;
    if (FAILED(t.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear,
                                                 IID_PPV_ARGS(&g.depth)))) {
        say_failed("its depth buffer could not be made.");
        return false;
    }
    g.depth->SetName(L"ReSkate viewmodel depth");
    t.device->CreateDepthStencilView(g.depth.Get(), nullptr, g.depth_view->GetCPUDescriptorHandleForHeapStart());
    g.depth_width = width;
    g.depth_height = height;
    return true;
}

bool ensure_constants(ID3D12Device* device) {
    auto& g = gpu();
    if (g.constants) return true;
    g.constants = buffer_in_upload_heap(device, constants_stride * constants_ring, L"ReSkate viewmodel constants");
    void* mapped{};
    const D3D12_RANGE none{};
    if (!g.constants || FAILED(g.constants->Map(0, &none, &mapped)) || !mapped) {
        g.constants.Reset();
        say_failed("its constants could not be made.");
        return false;
    }
    g.constants_mapped = static_cast<std::uint8_t*>(mapped);
    g.constants_fence = {};
    return true;
}

// A new model: its geometry (read by the GPU straight from upload memory: it is small), and its
// textures to come, one a frame.
bool take_model(const Target& t, std::shared_ptr<const Loaded> loaded) {
    auto& g = gpu();
    retire(g.vertices, t.fence);
    retire(g.indices, t.fence);
    for (auto& texture : g.textures) retire(texture, t.fence);
    g.textures.clear();
    g.next_texture = 0;
    g.views_made = 0;
    g.model = std::move(loaded);
    if (!g.model) return false;
    const auto& model = g.model->model;
    const std::size_t vertex_bytes = model.vertices.size() * sizeof(Vertex), index_bytes = model.indices.size() * 4;
    g.vertices = buffer_in_upload_heap(t.device, vertex_bytes, L"ReSkate viewmodel vertices");
    g.indices = buffer_in_upload_heap(t.device, index_bytes, L"ReSkate viewmodel indices");
    if (!g.vertices || !g.indices || !fill(g.vertices.Get(), model.vertices.data(), vertex_bytes) || !fill(g.indices.Get(), model.indices.data(), index_bytes)) {
        g.model.reset();
        g.vertices.Reset();
        g.indices.Reset();
        say_failed("its model could not be put on the GPU.");
        return false;
    }
    g.vertex_view = {g.vertices->GetGPUVirtualAddress(), static_cast<UINT>(vertex_bytes), sizeof(Vertex)};
    g.index_view = {g.indices->GetGPUVirtualAddress(), static_cast<UINT>(index_bytes), DXGI_FORMAT_R32_UINT};
    g.textures.resize(g.model->pictures.size());
    return true;
}

// The next of the model's pictures as a texture with all its levels; true when every one is.
bool upload_next_texture(const Target& t) {
    auto& g = gpu();
    while (g.next_texture < g.textures.size() && g.model->pictures[g.next_texture].rgba.empty()) ++g.next_texture;
    if (g.next_texture >= g.textures.size()) return true;
    const auto& picture = g.model->pictures[g.next_texture];
    D3D12_RESOURCE_DESC texture{};
    texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture.Width = picture.width;
    texture.Height = picture.height;
    texture.DepthOrArraySize = 1;
    texture.MipLevels = static_cast<UINT16>(picture.levels);
    texture.Format = picture.srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
    texture.SampleDesc.Count = 1;
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts(picture.levels);
    std::vector<UINT> rows(picture.levels);
    std::vector<UINT64> row_bytes(picture.levels);
    UINT64 total{};
    t.device->GetCopyableFootprints(&texture, 0, picture.levels, 0, layouts.data(), rows.data(), row_bytes.data(), &total);
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    ComPtr<ID3D12Resource> made;
    auto upload = buffer_in_upload_heap(t.device, static_cast<std::size_t>(total), L"ReSkate viewmodel picture upload");
    if (!upload || FAILED(t.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &texture, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                            IID_PPV_ARGS(&made)))) {
        say_failed("a picture could not be put on the GPU.");
        return false;
    }
    made->SetName(L"ReSkate viewmodel picture");
    void* mapped{};
    const D3D12_RANGE none{};
    if (FAILED(upload->Map(0, &none, &mapped)) || !mapped) {
        say_failed("a picture could not be put on the GPU.");
        return false;
    }
    std::size_t at = 0;
    UINT width = picture.width, height = picture.height;
    for (UINT level = 0; level < picture.levels; ++level) {
        for (UINT row = 0; row < height; ++row)
            std::memcpy(static_cast<std::uint8_t*>(mapped) + layouts[level].Offset + std::size_t{row} * layouts[level].Footprint.RowPitch,
                        picture.rgba.data() + at + std::size_t{row} * width * 4, std::size_t{width} * 4);
        at += std::size_t{width} * height * 4;
        width = std::max(1u, width / 2);
        height = std::max(1u, height / 2);
    }
    upload->Unmap(0, nullptr);
    for (UINT level = 0; level < picture.levels; ++level) {
        D3D12_TEXTURE_COPY_LOCATION to{}, from{};
        to.pResource = made.Get();
        to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        to.SubresourceIndex = level;
        from.pResource = upload.Get();
        from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        from.PlacedFootprint = layouts[level];
        t.commands->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    }
    transition(t.commands, made.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    g.textures[g.next_texture] = std::move(made);
    retire(upload, t.fence);
    ++g.next_texture;
    while (g.next_texture < g.textures.size() && g.model->pictures[g.next_texture].rgba.empty()) ++g.next_texture;
    return g.next_texture >= g.textures.size();
}

// The textures' views in the overlay's heap: a null view where a material has no such picture.
void make_views(const Target& t) {
    auto& g = gpu();
    if (g.view_heap != t.srvs || g.view_slot != t.slot) g.views_made = 0;
    if (g.views_made == g.textures.size() && g.views_made) return;
    const auto step = t.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for (std::size_t i = 0; i < g.textures.size() && i < t.slots; ++i) {
        auto cpu = t.srvs->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += static_cast<SIZE_T>(step) * (t.slot + i);
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Format = i % 3 == 0 || (g.model->model.materials[i/3].faces&64) ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
        view.Texture2D.MipLevels = g.textures[i] ? g.textures[i]->GetDesc().MipLevels : 1;
        t.device->CreateShaderResourceView(g.textures[i].Get(), &view, cpu);
    }
    g.views_made = g.textures.size();
    g.view_heap = t.srvs;
    g.view_slot = t.slot;
}

void free_retired(const Target& t) {
    auto& g = gpu();
    if (g.retired.empty() || !t.gpu_done) return;
    const auto done = t.gpu_done();
    std::erase_if(g.retired, [&](const Retired& r) { return r.fence <= done; });
}

void upload_prepared(const Target& t) {
    std::shared_ptr<const Loaded> loaded;
    std::uint64_t serial{};
    {
        auto& s=shared();std::lock_guard lock(s.mutex);
        loaded=s.prepared;serial=s.prepared_serial;
    }
    GpuScope scope(prepared_gpu());auto& g=gpu();free_retired(t);
    try {
        if (g.model!=loaded) {
            if (!loaded) {take_model(t,{});g.failed=false;return;}
            g.failed=false;
            if (!take_model(t,loaded)) throw std::runtime_error("prepared geometry upload failed");
            g.upload_fence=t.fence;
        }
        if (!loaded) return;
        if (g.next_texture<g.textures.size()) {
            upload_next_texture(t);g.upload_fence=t.fence;
        }
        if (g.failed) throw std::runtime_error("prepared texture upload failed");
        const bool ready=g.next_texture>=g.textures.size()&&t.gpu_done&&t.gpu_done()>=g.upload_fence;
        auto& s=shared();std::lock_guard lock(s.mutex);
        if (s.prepared_serial==serial&&s.prepared==loaded) s.prepared_ready=ready;
    } catch (const std::exception& error) {
        auto& s=shared();std::lock_guard lock(s.mutex);
        if (s.prepared_serial==serial) {s.prepared_status=Status::failed;s.prepared_why=error.what();s.prepared_ready=false;}
    }
}

std::array<float, 3> unit(std::array<float, 3> v, std::array<float, 3> fallback) {
    const float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (!(length > 1e-5f) || !std::isfinite(length)) return fallback;
    return {v[0] / length, v[1] / length, v[2] / length};
}
} // namespace

void load(const std::filesystem::path& file, const std::vector<std::filesystem::path>& attachments) {
    auto& s = shared();
    std::uint64_t serial{};
    {
        std::lock_guard lock(s.mutex);
        serial = ++s.serial;
        ++s.prepared_serial;s.prepared_status=Status::none;s.prepared.reset();s.next_read.reset();s.prepared_ready=false;
        s.status = Status::loading;
        s.render_ready=false;
        s.why.clear();
        s.loaded.reset();
        s.animator.stop();
    }
    std::thread(read_in_background, file, attachments, serial, false).detach();
}

void prepare(const std::filesystem::path& file,const std::vector<std::filesystem::path>& attachments) {
    auto& s=shared();bool launch=false;
    {
        std::lock_guard lock(s.mutex);
        const auto serial=++s.prepared_serial;s.prepared_ready=false;s.prepared_why.clear();s.prepared.reset();
        s.prepared_status=file.empty()?Status::none:Status::loading;
        if (file.empty()) s.next_read.reset();
        else {s.next_read=ReadRequest{file,attachments,serial};if(!s.preparing){s.preparing=true;launch=true;}}
    }
    if (launch) std::thread(prepare_worker).detach();
}

std::string activate_prepared() {
    auto& s=shared();std::lock_guard lock(s.mutex);
    if (s.prepared_status!=Status::ready||!s.prepared||!s.prepared_ready) return "prepared viewmodel is not GPU-ready";
    ++s.serial;s.loaded=std::move(s.prepared);s.status=Status::ready;s.render_ready=false;s.why.clear();s.animator.stop();
    ++s.prepared_serial;s.prepared_status=Status::none;s.prepared_ready=false;s.prepared_why.clear();s.next_read.reset();
    return {};
}

void unload() {
    display({},std::nullopt);
    scene_depth().wanted.store(false, std::memory_order_relaxed);
    auto& s = shared();
    std::lock_guard lock(s.mutex);
    ++s.serial;
    ++s.prepared_serial;s.prepared_status=Status::none;s.prepared.reset();s.next_read.reset();s.prepared_ready=false;
    s.status = Status::none;
    s.why.clear();
    s.loaded.reset();
    s.render_ready=false;
    s.until = 0;
    s.animator.stop();
}

void display(const std::filesystem::path& file,const std::optional<Display>& look) {
    auto& state = display_shared();
    {
        std::lock_guard lock(state.mutex);
        state.look = look;
        state.until = look ? steady_seconds() + .3 : 0;
        if (look) {
            scene_depth().deadline.store(state.until, std::memory_order_relaxed);
            if (state.file == file) return;
        }
        ++state.serial;
        state.file = look ? file : std::filesystem::path{};
        state.loaded.reset();
        state.status = look ? Status::loading : Status::none;
        state.why.clear();
        state.pending = look.has_value();
        if (!state.pending || state.reading) return;
        state.reading = true;
    }
    std::thread([] {
        auto& display_state = display_shared();
        for (;;) {
            std::filesystem::path path;
            std::uint64_t serial;
            {
                std::lock_guard lock(display_state.mutex);
                if (!display_state.pending) {
                    display_state.reading = false;
                    return;
                }
                path = display_state.file;
                serial = display_state.serial;
                display_state.pending = false;
            }
            std::shared_ptr<Loaded> loaded;
            std::string why;
            try {
                std::size_t picture_bytes = 0;
                loaded = std::make_shared<Loaded>();
                if (!read_one(path, *loaded, picture_bytes, why)) loaded.reset();
            } catch (const std::exception&) {
                loaded.reset();
                why = "world display could not be read within its resource limits";
            }
            std::lock_guard lock(display_state.mutex);
            if (display_state.serial != serial) continue;
            display_state.status = loaded ? Status::ready : Status::failed;
            display_state.loaded = std::move(loaded);
            display_state.why = std::move(why);
        }
    }).detach();
}
std::optional<Display> display_now() {
    auto& d=display_shared();std::lock_guard lock(d.mutex);
    return steady_seconds()<d.until?d.look:std::nullopt;
}

Info info() {
    auto& s = shared();
    std::lock_guard lock(s.mutex);
    Info out;
    {auto& d=display_shared();std::lock_guard display_lock(d.mutex);out.display_status=d.status;out.display_why=d.why;}
    out.status = s.status;
    out.why = s.why;
    out.render_ready=s.render_ready;
    out.prepared_status=s.prepared_status;out.prepared_why=s.prepared_why;out.prepared_ready=s.prepared_ready;
    if (s.loaded) {
        for (const auto& clip : s.loaded->model.clips) out.clips.push_back({clip.name, clip.duration()});
        for (const auto& bone : s.loaded->model.bones) out.bones.push_back(bone.name);
    }
    return out;
}

void show(const Look& look) noexcept {
    auto& s = shared();
    std::lock_guard lock(s.mutex);
    s.look = look;
    s.until = steady_seconds() + 0.3;
    scene_depth().deadline.store(s.until, std::memory_order_relaxed);
}
void hide() noexcept {
    if (!display_now()) { auto& scene = scene_depth(); std::lock_guard lock(scene.mutex); scene.wanted = false; }
    auto& s = shared();
    std::lock_guard lock(s.mutex);
    s.until = 0;
}

std::string play(std::string_view clip, const Animator::Play& play) {
    auto& s = shared();
    std::lock_guard lock(s.mutex);
    if (!s.loaded) return s.status == Status::loading ? "the viewmodel is still loading" : "no viewmodel is loaded";
    const int index = s.loaded->model.clip(clip);
    if (index < 0) return "the viewmodel has no clip \"" + std::string(clip) + "\"";
    auto asked = play;
    asked.clip = index;
    s.animator.play(s.loaded->model, asked, steady_seconds());
    return {};
}
void stop() noexcept {
    auto& s = shared();
    std::lock_guard lock(s.mutex);
    s.animator.stop();
}

std::string seek(double seconds) {
    auto& s=shared();std::lock_guard lock(s.mutex);
    if (!s.loaded || !s.animator.seek(seconds,steady_seconds())) return "no animation is playing or seek time is invalid";
    return {};
}

State state() {
    auto& s = shared();
    std::lock_guard lock(s.mutex);
    if (!s.loaded) return {};
    const auto now_state = s.animator.state(s.loaded->model, steady_seconds());
    if (now_state.clip < 0) return {};
    const auto& clip = s.loaded->model.clips[static_cast<std::size_t>(now_state.clip)];
    return {clip.name, now_state.time, clip.duration(), now_state.done};
}

// Resolve world geometry through the same camera for drawing and bone queries.
std::optional<Affine> resolve_look(Look& look) {
    if (!look.world_position) return placement(look.offset,look.yaw,look.pitch,look.roll);
    auto camera=plugin_camera::camera_now();
    if (look.view_position) {
        const auto forward=unit(look.view_forward,{0,0,1});
        const auto& up0=look.view_up;
        const auto right=unit(std::array<float,3>{forward[1]*up0[2]-forward[2]*up0[1],forward[2]*up0[0]-forward[0]*up0[2],forward[0]*up0[1]-forward[1]*up0[0]},{-1,0,0});
        const std::array<float,3> up{right[1]*forward[2]-right[2]*forward[1],right[2]*forward[0]-right[0]*forward[2],right[0]*forward[1]-right[1]*forward[0]};
        GameView paired;const auto& at=*look.view_position;
        paired.world={right[0],right[1],right[2],0,up[0],up[1],up[2],0,-forward[0],-forward[1],-forward[2],0,at[0],at[1],at[2],1};
        paired.vertical_fov=look.view_fov;camera=paired;
    }
    if (!camera) return std::nullopt;
    const auto& m=camera->world;
    const auto view = [&](const std::array<float,3>& v) {
        return std::array<float,3>{v[0]*m[0]+v[1]*m[1]+v[2]*m[2], v[0]*m[4]+v[1]*m[5]+v[2]*m[6], -v[0]*m[8]-v[1]*m[9]-v[2]*m[10]};
    };
    const auto cross = [](const std::array<float,3>& a, const std::array<float,3>& b) {
        return std::array<float,3>{a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};
    };
    const auto z = unit(look.world_forward,{0,0,1});
    const auto x = unit(cross(z,look.world_up),{-1,0,0});
    const auto y = cross(x,z);
    const auto vx = view(x), vy = view(y), vz = view(z);
    const auto& p = *look.world_position;
    const auto origin_view = view({p[0]-m[12],p[1]-m[13],p[2]-m[14]});
    for (auto& point : look.depth_points) point = view({point[0]-m[12],point[1]-m[13],point[2]-m[14]});
    look.light = view(look.light);
    look.ambient_up = view({0,1,0});
    if (look.contact_position) {
        const auto p0 = *look.contact_position;
        look.contact_position = view({p0[0]-m[12],p0[1]-m[13],p0[2]-m[14]});
        look.contact_normal = view(unit(look.contact_normal,{0,1,0}));
    }
    if (look.reflection) {
        auto& r=*look.reflection;const auto point=r.position;
        r.position=view({point[0]-m[12],point[1]-m[13],point[2]-m[14]});r.normal=view(unit(r.normal,{0,1,0}));
    }
    if (look.clip_position) {
        const auto point=*look.clip_position;
        look.clip_position=view({point[0]-m[12],point[1]-m[13],point[2]-m[14]});look.clip_normal=view(unit(look.clip_normal,{0,1,0}));
    }
    look.fov = camera->vertical_fov;
    std::erase_if(look.depth_points,[](const auto& point) { return point[2] <= .01f; });
    return Affine{vx[0],vy[0],vz[0],origin_view[0], vx[1],vy[1],vz[1],origin_view[1], vx[2],vy[2],vz[2],origin_view[2]};
}

std::optional<BonePose> bone(std::string_view name) {
    std::shared_ptr<const Loaded> loaded;
    Animator animator;
    Look look;
    {
        auto& s = shared();
        std::lock_guard lock(s.mutex);
        loaded = s.loaded;
        animator = s.animator;
        look = s.look;
    }
    if (!loaded) return std::nullopt;
    const int index = loaded->model.bone(name);
    if (index < 0) return std::nullopt;
    std::array<Affine, max_bones> skin;
    animator.pose(loaded->model, steady_seconds(), std::span(skin).first(loaded->model.bones.size()));
    if (look.pose_transform) for (std::size_t i=0;i<loaded->model.bones.size();++i) skin[i]=multiply(*look.pose_transform,skin[i]);
    for (const auto& [bone_name, matrix] : look.transforms) {
        const int bone_index = loaded->model.bone(bone_name);
        if (bone_index >= 0) skin[static_cast<std::size_t>(bone_index)] = matrix;
    }
    const auto resolved = resolve_look(look);
    if (!resolved) return std::nullopt;
    const auto pose = multiply(*resolved,
                               multiply(skin[static_cast<std::size_t>(index)], loaded->model.bones[static_cast<std::size_t>(index)].bind));
    BonePose out;
    for (int r = 0; r < 3; ++r) {
        out.x[r] = pose[r * 4 + 0];
        out.y[r] = pose[r * 4 + 1];
        out.z[r] = pose[r * 4 + 2];
        out.position[r] = pose[r * 4 + 3];
    }
    return out;
}

bool pending() noexcept {
    if (display_now()) return true;
    auto& s = shared();
    std::lock_guard lock(s.mutex);
    if (s.prepared_status==Status::loading||(s.prepared_status==Status::ready&&!s.prepared_ready)) return true;
    if (prepared_gpu().model&&!s.prepared) return true;
    if (gpu().failed || !s.loaded) return false;
    return steady_seconds() < s.until || gpu().model != s.loaded || gpu().next_texture < gpu().textures.size();
}

void capture_depth(ID3D12GraphicsCommandList* list, const D3D12_RESOURCE_BARRIER& b,
    void (STDMETHODCALLTYPE* original)(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*)) noexcept {
    try {
        auto& s = scene_depth();
        if (!s.wanted.load(std::memory_order_relaxed)) return;
        if (steady_seconds() >= s.deadline.load(std::memory_order_relaxed)) {
            s.wanted.store(false, std::memory_order_relaxed);
            return;
        }
        std::lock_guard lock(s.mutex);
        if (!s.wanted || s.copied || !s.texture || b.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION || !b.Transition.pResource ||
            (b.Flags & D3D12_RESOURCE_BARRIER_FLAG_END_ONLY) || !(b.Transition.StateBefore & D3D12_RESOURCE_STATE_DEPTH_WRITE) ||
            !(b.Transition.StateAfter & (D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)) ||
            (b.Transition.Subresource != 0 && b.Transition.Subresource != D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) ||
            list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT) return;
        const auto desc = b.Transition.pResource->GetDesc();
        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.Width != s.width || desc.Height != s.height ||
            desc.SampleDesc.Count != 1 || desc.DepthOrArraySize != 1 ||
            (desc.Format != DXGI_FORMAT_R32G8X24_TYPELESS && desc.Format != DXGI_FORMAT_D32_FLOAT_S8X24_UINT &&
             desc.Format != DXGI_FORMAT_R32_TYPELESS && desc.Format != DXGI_FORMAT_D32_FLOAT)) return;
        const auto family = desc.Format == DXGI_FORMAT_R32_TYPELESS || desc.Format == DXGI_FORMAT_D32_FLOAT
            ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_R32G8X24_TYPELESS;
        s.wanted_format = family;
        if (s.format != family) return;
        D3D12_RESOURCE_BARRIER barriers[2]{};
        barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[0].Transition = b.Transition;
        barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[1].Transition.pResource = s.texture.Get();
        barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        original(list, 2, barriers);
        D3D12_TEXTURE_COPY_LOCATION to{}, from{};
        to.pResource = s.texture.Get(); to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        from.pResource = b.Transition.pResource; from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        for (auto& barrier : barriers) std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        original(list, 2, barriers);
        s.copied = s.valid = true;
    } catch (...) {}
}

static bool record_scene(const Target& t,const Look* override_look=nullptr,
    std::shared_ptr<const Loaded> display_model={},const Display* display_look=nullptr) noexcept {
    try {
        auto& g = gpu();
        if (g.failed || !t.device || !t.commands || !t.buffer || !t.srvs) return false;
        std::shared_ptr<const Loaded> loaded;
        Animator animator;
        Look look;
        bool shown{};
        const double at = steady_seconds();
        {
            auto& s = shared();
            std::lock_guard lock(s.mutex);
            loaded = s.loaded;
            animator = s.animator;
            look = s.look;
            shown = at < s.until;
        }
        if (override_look) look=*override_look;
        if (display_look) {loaded=std::move(display_model);animator.stop();shown=true;}
        if (!override_look) {
            if (loaded&&prepared_gpu().model==loaded) std::swap(g,prepared_gpu());
            upload_prepared(t);
        }
        const auto resolved_place = resolve_look(look);
        if (!resolved_place) return false;
        free_retired(t);
        if (g.model != loaded && !take_model(t, loaded)) return false;
        if (!g.model) return false;
        // Its pictures go up one a frame (each is up to 2048 pixels a side with its levels), and it is
        // drawn once they all have.
        if (g.next_texture < g.textures.size() && !upload_next_texture(t)) return false;
        if (!shown || g.model->model.materials.size() * 3 + 1 > t.slots) return false;
        make_views(t);
        int screen_material=-1;
        if (display_look && display_look->live && surface().colour) {
            const auto& materials=g.model->model.materials;
            for (std::size_t i=0;i<materials.size();++i) if(materials[i].name==display_look->material) {
                screen_material=static_cast<int>(i);
                auto cpu=t.srvs->GetCPUDescriptorHandleForHeapStart();
                cpu.ptr+=static_cast<SIZE_T>(t.slot+i*3)*t.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
                srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Texture2D.MipLevels=1;
                t.device->CreateShaderResourceView(surface().colour.Get(),&srv,cpu);
            }
        }
        const auto desc = t.buffer->GetDesc();
        if (desc.Width < 16 || desc.Height < 16) return false;
        if (!build_pipeline(t.device, t.format) || !ensure_depth(t, desc.Width, desc.Height) || !ensure_constants(t.device)) return false;
        // This frame's constants, in a part of the ring the GPU has finished with.
        const std::size_t ring = g.constants_next;
        if (t.gpu_done && t.gpu_done() < g.constants_fence[ring]) return false;
        g.constants_next = (ring + 1) % constants_ring;
        g.constants_fence[ring] = t.fence;
        const auto& model = g.model->model;
        std::array<float, frame_floats> frame{};
        frame[84]=look.clip_circle[0]*static_cast<float>(desc.Width);
        frame[85]=look.clip_circle[1]*static_cast<float>(desc.Height);
        frame[86]=look.clip_circle[2]*static_cast<float>(desc.Width);
        frame[87]=look.clip_circle[3]*static_cast<float>(desc.Height);
        std::copy(look.projection_shift.begin(),look.projection_shift.end(),frame.begin()+88);
        std::copy(look.fog.begin(),look.fog.end(),frame.begin()+92);
        std::copy(look.fog_colour.begin(),look.fog_colour.end(),frame.begin()+96);
        if (look.reflection) {
            const auto& r=*look.reflection;
            std::copy(r.normal.begin(),r.normal.end(),frame.begin()+100);
            frame[103]=-(r.normal[0]*r.position[0]+r.normal[1]*r.position[1]+r.normal[2]*r.position[2]);
            frame[104]=r.time;frame[105]=r.distortion;frame[106]=r.strength;frame[107]=r.texture_scale;
            std::copy(r.tint.begin(),r.tint.end(),frame.begin()+108);frame[111]=1;
        }
        if (look.clip_position) {
            std::copy(look.clip_normal.begin(),look.clip_normal.end(),frame.begin()+112);
            const auto& n=look.clip_normal;const auto& p=*look.clip_position;
            frame[115]=-(n[0]*p[0]+n[1]*p[1]+n[2]*p[2]);
        }
        const float aspect = look.projection_aspect>0?look.projection_aspect:static_cast<float>(desc.Width) / static_cast<float>(desc.Height);
        const float fov = std::clamp(look.fov, 20.0f, 120.0f) * std::numbers::pi_v<float> / 180.0f;
        const float focal = 1.0f / std::tan(fov * 0.5f);
        const float depth_scale = -near_plane / (far_plane - near_plane);
        const auto& place = *resolved_place;
        const auto light = unit(look.light, {-0.35f, 0.75f, -0.55f});
        const std::array<float, 12 * 4> header{
            focal / aspect, focal, depth_scale, -far_plane * depth_scale,
            place[0], place[1], place[2], place[3], place[4], place[5], place[6], place[7], place[8], place[9], place[10], place[11],
            light[0], light[1], light[2], 0,
            look.light_colour[0], look.light_colour[1], look.light_colour[2], std::clamp(look.ambient, 0.0f, 4.0f),
            look.sky[0], look.sky[1], look.sky[2], std::clamp(look.brightness, 0.0f, 4.0f),
            look.ground[0], look.ground[1], look.ground[2], 0,
            static_cast<float>(t.transfer), t.white_nits, 0, 0,
            look.ambient_up[0], look.ambient_up[1], look.ambient_up[2], 0,
            look.contact_normal[0], look.contact_normal[1], look.contact_normal[2],
                look.contact_position ? -((*look.contact_position)[0]*look.contact_normal[0]+(*look.contact_position)[1]*look.contact_normal[1]+(*look.contact_position)[2]*look.contact_normal[2]) : 0,
            look.contact_position ? look.contact_margin : 0, look.scene_clear ? 1.0f : 0.0f, 0, 0};
        std::copy(header.begin(), header.end(), frame.begin());
        auto& scene = scene_depth();
        {
            std::lock_guard lock(scene.mutex);
            // An offscreen source must not disarm the world's depth capture.
            if (!display_now() || display_look) scene.wanted = look.scene_depth;
            if (look.scene_depth && (!scene.texture || scene.width != desc.Width || scene.height != desc.Height || scene.format != scene.wanted_format)) {
                retire(scene.texture, t.fence);
                auto depth_desc = desc;
                scene.format = scene.wanted_format;
                depth_desc.Format = scene.format;
                depth_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
                depth_desc.MipLevels = 1;
                D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
                scene.valid = false;
                if (FAILED(t.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &depth_desc,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    nullptr, IID_PPV_ARGS(&scene.texture)))) return false;
                scene.width = desc.Width; scene.height = desc.Height;
            }
            const auto slot = t.slot + t.slots - 1;
            auto cpu = t.srvs->GetCPUDescriptorHandleForHeapStart();
            cpu.ptr += static_cast<SIZE_T>(slot) * t.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Format = scene.format == DXGI_FORMAT_R32G8X24_TYPELESS ? DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS : DXGI_FORMAT_R32_FLOAT; srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; srv.Texture2D.MipLevels = 1;
            t.device->CreateShaderResourceView(scene.texture.Get(), &srv, cpu);
            frame[48] = look.scene_depth ? 1.0f : 0.0f;
            frame[49] = scene.valid && scene.copied ? static_cast<float>(look.depth_points.size()) : 0.0f;
            frame[50] = static_cast<float>(desc.Width); frame[51] = static_cast<float>(desc.Height);
            for (std::size_t i = 0; i < look.depth_points.size(); ++i)
                std::copy(look.depth_points[i].begin(), look.depth_points[i].end(), frame.begin() + 52 + i * 4);
            if (look.scene_depth) scene.copied = false;
        }
        std::array<Affine, max_bones> skin;
        animator.pose(model, at, std::span(skin).first(model.bones.size()));
        if (look.pose_transform) for (std::size_t i=0;i<model.bones.size();++i) skin[i]=multiply(*look.pose_transform,skin[i]);
        for (const auto& [name, matrix] : look.transforms) {
            const int index = model.bone(name);
            if (index >= 0) skin[static_cast<std::size_t>(index)] = matrix;
        }
        for (std::size_t i = 0; i < model.bones.size(); ++i) std::copy(skin[i].begin(), skin[i].end(), frame.begin() + frame_header_floats + i * 12);
        std::memcpy(g.constants_mapped + ring * constants_stride, frame.data(), sizeof frame);

        transition(t.commands, t.buffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        if (look.backdrop) {
            const auto& colour=*look.backdrop;
            float clear[4]{colour[0],colour[1],colour[2],1};
            if (t.transfer>0) {
                for (int i=0;i<3;++i) clear[i]=colour[i]<=.04045f?colour[i]/12.92f:std::pow((colour[i]+.055f)/1.055f,2.4f);
                if (t.transfer==1) for (int i=0;i<3;++i) clear[i]*=t.white_nits/80;
                else {
                    const std::array<float,3> wide{.6274f*clear[0]+.3293f*clear[1]+.0433f*clear[2],
                        .0691f*clear[0]+.9195f*clear[1]+.0114f*clear[2],.0164f*clear[0]+.0880f*clear[1]+.8956f*clear[2]};
                    for (int i=0;i<3;++i) {
                        const float y=std::pow(std::clamp(wide[i]*t.white_nits/10000,0.0f,1.0f),.1593017578125f);
                        clear[i]=std::pow((.8359375f+18.8515625f*y)/(1+18.6875f*y),78.84375f);
                    }
                }
            }
            t.commands->ClearRenderTargetView(t.rtv,clear,0,nullptr);
        }
        const auto depth = g.depth_view->GetCPUDescriptorHandleForHeapStart();
        t.commands->OMSetRenderTargets(1, &t.rtv, FALSE, &depth);
        t.commands->ClearDepthStencilView(depth, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
        const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(desc.Width), static_cast<float>(desc.Height), 0.0f, 1.0f};
        D3D12_RECT scissor{0, 0, static_cast<LONG>(desc.Width), static_cast<LONG>(desc.Height)};
        if (frame[86]>0) {
            scissor.left=static_cast<LONG>(std::clamp(std::floor(frame[84]-frame[86]),0.0f,static_cast<float>(desc.Width)));
            scissor.right=static_cast<LONG>(std::clamp(std::ceil(frame[84]+frame[86]),0.0f,static_cast<float>(desc.Width)));
            scissor.top=static_cast<LONG>(std::clamp(std::floor(frame[85]-frame[87]),0.0f,static_cast<float>(desc.Height)));
            scissor.bottom=static_cast<LONG>(std::clamp(std::ceil(frame[85]+frame[87]),0.0f,static_cast<float>(desc.Height)));
        }
        t.commands->RSSetViewports(1, &viewport);
        t.commands->RSSetScissorRects(1, &scissor);
        t.commands->SetGraphicsRootSignature(g.root.Get());
        t.commands->SetPipelineState(g.pipelines[0].Get());
        auto scene_table = t.srvs->GetGPUDescriptorHandleForHeapStart();
        scene_table.ptr += static_cast<UINT64>(t.slot + t.slots - 1) * t.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        t.commands->SetGraphicsRootDescriptorTable(3, scene_table);
        const auto planar_slot=t.slot+t.slots-(look.reflection?5u:3u);
        for (unsigned i=0;i<2;++i) {
            auto cpu=t.srvs->GetCPUDescriptorHandleForHeapStart();cpu.ptr+=static_cast<SIZE_T>(planar_slot+i)*t.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Texture2D.MipLevels=1;
            t.device->CreateShaderResourceView(look.reflection?g.planar[i].Get():nullptr,&srv,cpu);
        }
        auto planar_table=t.srvs->GetGPUDescriptorHandleForHeapStart();
        planar_table.ptr+=static_cast<UINT64>(planar_slot)*t.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        t.commands->SetGraphicsRootDescriptorTable(5,planar_table);
        t.commands->SetGraphicsRootConstantBufferView(1, g.constants->GetGPUVirtualAddress() + ring * constants_stride);
        t.commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        if (frame[86]>0&&look.projection_shift[2]>0) {
            const std::array<float,24> empty{};
            t.commands->SetGraphicsRoot32BitConstants(0,24,empty.data(),0);
            t.commands->SetGraphicsRoot32BitConstants(4,12,identity.data(),0);
            auto texture=t.srvs->GetGPUDescriptorHandleForHeapStart();
            texture.ptr+=static_cast<UINT64>(t.slot)*t.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            t.commands->SetGraphicsRootDescriptorTable(2,texture);
            t.commands->SetPipelineState(g.inset_background.Get());t.commands->DrawInstanced(3,1,0,0);
            t.commands->SetPipelineState(g.pipelines[0].Get());
        }
        t.commands->IASetVertexBuffers(0, 1, &g.vertex_view);
        t.commands->IASetIndexBuffer(&g.index_view);
        const auto step = t.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        std::unordered_map<std::string_view,const Affine*> part_overrides;
        for (const auto& [name,matrix]:look.part_transforms) part_overrides[name]=&matrix;
        const std::unordered_set<std::string_view> hidden(look.hidden_parts.begin(),look.hidden_parts.end());
        const std::unordered_set<std::string_view> fog_exempt(look.fog_exempt_parts.begin(),look.fog_exempt_parts.end());
        std::unordered_map<std::string_view,std::array<float,3>> effects;
        for (const auto& [name,value]:look.part_effects) effects[name]=value;
        const auto opacity_of=[&](const Part& part) {
            const auto& material=model.materials[part.material];float opacity=material.opacity;
            for (const auto& [name,value]:look.opacities) if (name==material.name) opacity*=value;
            if (const auto e=effects.find(part.name);e!=effects.end()) opacity*=e->second[0];
            return opacity;
        };
        const auto depth_of=[&](const Part& part) {
            const auto found=part_overrides.find(part.name);
            const auto& object=found==part_overrides.end()?part.transform:*found->second;
            auto matrix=object;
            if (part.bone_hint>=0) matrix=multiply(skin[static_cast<std::size_t>(part.bone_hint)],matrix);
            else if (look.pose_transform) matrix=multiply(*look.pose_transform,matrix);
            matrix=multiply(place,matrix);
            return matrix[8]*part.centre[0]+matrix[9]*part.centre[1]+matrix[10]*part.centre[2]+matrix[11];
        };
        std::vector<const Part*> ordered;ordered.reserve(model.parts.size());
        for (const auto& part:model.parts) if (part.name.empty() || !hidden.contains(part.name)) ordered.push_back(&part);
        // Attachments share depth: opaque geometry precedes all glass, then transparent
        // parts blend back to front across the whole bundle.
        std::stable_sort(ordered.begin(),ordered.end(),[&](const Part* a,const Part* b) {
            const bool at=opacity_of(*a)<1,bt=opacity_of(*b)<1;
            if (at!=bt) return !at;
            return at && depth_of(*a)>depth_of(*b);
        });
        for (const auto* pointer : ordered) {
            const auto& part=*pointer;
            if (!part.name.empty() && hidden.contains(part.name)) continue;
            const auto found=part_overrides.find(part.name);
            const auto& object=found==part_overrides.end()?part.transform:*found->second;
            t.commands->SetGraphicsRoot32BitConstants(4,12,object.data(),0);
            const auto& material = model.materials[part.material];
            const std::size_t first = std::size_t{part.material} * 3;
            const bool screen=static_cast<int>(part.material)==screen_material;
            const bool reflective=look.reflection && std::find(look.reflection->parts.begin(),look.reflection->parts.end(),part.name)!=look.reflection->parts.end();
            const unsigned planar_flag = !reflective ? 0u : look.reflection->portal_views.empty() ? 1024u :
                (part.name==look.reflection->parts[0] ? 2048u : 4096u);
            const unsigned has = (material.extended ? 8192u : 0u) | planar_flag | (screen?65u:0u) | ((material.faces & 128) ? 512u : 0u) | ((material.faces & 3) == 0 ? 256u : 0u) | ((material.faces & 64) ? 128u : 0u) | ((material.faces & 32) ? 64u : 0u) | ((material.faces & 16) ? 32u : 0u) | ((material.faces & 8) ? 16u : 0u) | ((material.faces & 4) ? 8u : 0u) | (g.textures[first] ? 1u : 0u) | (g.textures[first + 1] ? 2u : 0u) | (g.textures[first + 2] ? 4u : 0u);
            const float opacity=opacity_of(part);
            t.commands->SetPipelineState(g.pipelines[(material.faces & 3) + (opacity < 1 ? 3 : 0)].Get());
            std::array<float,24> constants{material.roughness,material.specular,static_cast<float>(has),opacity};
            constants[20]=constants[21]=constants[22]=constants[23]=1;
            for (const auto& [name,colour]:look.material_colours) if (name==material.name)
                std::copy(colour.begin(),colour.end(),constants.begin()+20);
            if (fog_exempt.contains(part.name)) constants[18]=1;
            for (std::size_t i=0;i<3;++i) std::copy(material.layers[i].begin(),material.layers[i].end(),constants.begin()+4+i*4);
            if (const auto e=effects.find(part.name);e!=effects.end()) {constants[16]=e->second[1];constants[17]=e->second[2];}
            t.commands->SetGraphicsRoot32BitConstants(0,24,constants.data(),0);
            auto table = t.srvs->GetGPUDescriptorHandleForHeapStart();
            table.ptr += static_cast<UINT64>(step) * (t.slot + first);
            t.commands->SetGraphicsRootDescriptorTable(2, table);
            t.commands->DrawIndexedInstanced(part.count, 1, part.first, 0, 0);
        }
        transition(t.commands, t.buffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        if (!display_look) {auto& s=shared();std::lock_guard lock(s.mutex);if(s.loaded==g.model) s.render_ready=true;}
        return true;
    } catch (...) {
        say_failed("an exception was raised while drawing it.");
        return false;
    }
}

static void record_planar(const Target& t,const Look& source) {
    auto& g=gpu();
    if (!source.reflection || !source.view_position) return;
    const auto r=*source.reflection;
    if (!g.planar[0]) {
        D3D12_DESCRIPTOR_HEAP_DESC views{};views.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;views.NumDescriptors=2;
        if (FAILED(t.device->CreateDescriptorHeap(&views,IID_PPV_ARGS(&g.planar_rtvs)))) return;
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=512;desc.Height=256;
        desc.DepthOrArraySize=desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Format=DXGI_FORMAT_R8G8B8A8_TYPELESS;desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        for (unsigned i=0;i<2;++i) {
            if (FAILED(t.device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,nullptr,IID_PPV_ARGS(&g.planar[i])))) {
                g.planar[0].Reset();g.planar[1].Reset();return;
            }
            auto cpu=g.planar_rtvs->GetCPUDescriptorHandleForHeapStart();cpu.ptr+=i*t.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            D3D12_RENDER_TARGET_VIEW_DESC rtv{};rtv.Format=DXGI_FORMAT_R8G8B8A8_UNORM;rtv.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE2D;
            t.device->CreateRenderTargetView(g.planar[i].Get(),&rtv,cpu);
        }
    }
    CaptureScope scope(g);
    const auto normal=unit(r.normal,{0,1,0});
    const auto reflect=[&](std::array<float,3> value,bool point) {
        float distance{};for (unsigned i=0;i<3;++i) distance+=normal[i]*(value[i]-(point?r.position[i]:0));
        for (unsigned i=0;i<3;++i) value[i]-=2*distance*normal[i];return value;
    };
    for (unsigned i=0;i<2;++i) {
        Look capture=source;capture.reflection.reset();capture.inset.reset();capture.clip_circle={};capture.projection_shift={};
        capture.scene_depth=false;capture.depth_points.clear();capture.contact_position.reset();
        capture.projection_aspect=static_cast<float>(t.buffer->GetDesc().Width)/t.buffer->GetDesc().Height;
        for (const auto& name:r.parts) capture.hidden_parts.push_back(name);
        capture.clip_position=r.position;capture.clip_normal=normal;
        float side{};for (unsigned axis=0;axis<3;++axis) side+=normal[axis]*((*source.view_position)[axis]-r.position[axis]);
        if ((side<0)==(i==0)) for (auto& n:capture.clip_normal) n=-n;
        if (r.portal_views.size()==2) {
            const auto& portal=r.portal_views[i];
            capture.view_position=portal.eye;capture.view_forward=portal.forward;capture.view_up=portal.up;
            capture.clip_position=portal.exit;capture.clip_normal=portal.normal;
        } else if (i==0) {
            capture.view_position=reflect(*source.view_position,true);capture.view_forward=reflect(source.view_forward,false);capture.view_up=reflect(source.view_up,false);
        }
        Target output=t;output.buffer=g.planar[i].Get();output.rtv=g.planar_rtvs->GetCPUDescriptorHandleForHeapStart();
        output.rtv.ptr+=i*t.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        output.format=DXGI_FORMAT_R8G8B8A8_UNORM;output.transfer=0;output.white_nits=80;
        transition(t.commands,output.buffer,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_PRESENT);
        if (!record_scene(output,&capture)) {
            transition(t.commands,output.buffer,D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_RENDER_TARGET);
            constexpr float clear[4]{0,0,0,1};t.commands->ClearRenderTargetView(output.rtv,clear,0,nullptr);
            transition(t.commands,output.buffer,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PRESENT);
        }
        transition(t.commands,output.buffer,D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
}

static bool record_source(const Target& t) noexcept {
    try {
    Look primary;
    {auto& s=shared();std::lock_guard lock(s.mutex);primary=s.look;}
    // Prepared scenes keep their uploaded model/textures when a reflective
    // surface is the first pass that asks to draw them.
    {std::shared_ptr<const Loaded> loaded;{auto& s=shared();std::lock_guard lock(s.mutex);loaded=s.loaded;}
        if(loaded && prepared_gpu().model==loaded) std::swap(gpu(),prepared_gpu());}
    record_planar(t,primary);
    const bool drawn=record_scene(t);
    if (!drawn) return false;
    Look secondary;
    {auto& s=shared();std::lock_guard lock(s.mutex);secondary=s.look;}
    if (!secondary.inset) return true;
    const auto inset=*secondary.inset;secondary.inset.reset();secondary.backdrop.reset();secondary.reflection.reset();
    secondary.view_position=inset.position;secondary.view_forward=inset.forward;secondary.view_up=inset.up;secondary.view_fov=inset.fov;
    secondary.scene_depth=false;secondary.depth_points.clear();secondary.contact_position.reset();
    secondary.clip_circle={inset.centre[0],inset.centre[1],inset.radius[0],inset.radius[1]};
    secondary.projection_shift={inset.projection_offset[0],inset.projection_offset[1],inset.background_alpha};
    for (const auto& name:inset.hidden_bones) {
        std::erase_if(secondary.transforms,[&](const auto& entry){return entry.first==name;});
        secondary.transforms.emplace_back(name,Affine{});
    }
    for (const auto& name:inset.hidden_materials) secondary.opacities.emplace_back(name,0.0f);
    record_scene(t,&secondary);
    return true;
    } catch (...) { say_failed("an exception was raised while drawing the secondary view.");return false; }
}

bool record(const Target& target) noexcept {
    try {
        Target t=target;t.slots=std::min(t.slots,scene_descriptor_slots);
        const auto display_look=display_now();
        if (!display_look) {
            // A stalled world-display owner must not flash its source full screen.
            {auto& d=display_shared();std::lock_guard lock(d.mutex);if(d.look)return false;}
            return record_source(t);
        }
        std::shared_ptr<const Loaded> cabinet;
        {auto& d=display_shared();std::lock_guard lock(d.mutex);cabinet=d.loaded;}
        if (!cabinet || target.slots<descriptor_slots) {upload_prepared(t);return false;}
        auto& output=surface();
        if (display_look->live) {
            if (!output.colour) {
                D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
                D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                desc.Width=display_width;desc.Height=display_height;desc.DepthOrArraySize=desc.MipLevels=1;desc.SampleDesc.Count=1;
                desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
                if (FAILED(t.device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,nullptr,IID_PPV_ARGS(&output.colour)))) return false;
                D3D12_DESCRIPTOR_HEAP_DESC views{};views.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;views.NumDescriptors=1;
                if (FAILED(t.device->CreateDescriptorHeap(&views,IID_PPV_ARGS(&output.rtv)))) {output.colour.Reset();return false;}
                t.device->CreateRenderTargetView(output.colour.Get(),nullptr,output.rtv->GetCPUDescriptorHandleForHeapStart());
            }
            Target source=t;source.buffer=output.colour.Get();source.rtv=output.rtv->GetCPUDescriptorHandleForHeapStart();
            source.format=DXGI_FORMAT_R8G8B8A8_UNORM;source.transfer=0;source.white_nits=80;
            transition(t.commands,source.buffer,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_PRESENT);
            const bool drawn=record_source(source);
            if (!drawn) {
                transition(t.commands,source.buffer,D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_RENDER_TARGET);
                constexpr float black[4]{0,0,0,1};t.commands->ClearRenderTargetView(source.rtv,black,0,nullptr);
                transition(t.commands,source.buffer,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PRESENT);
            }
            if (target.draw_surface_overlay) target.draw_surface_overlay(source);
            transition(t.commands,source.buffer,D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        } else upload_prepared(t);
        GpuScope context(display_gpu());
        Look look;look.world_position=display_look->position;look.world_forward=display_look->forward;look.world_up=display_look->up;
        look.scene_depth=true;look.scene_clear=display_look->scene_clear;look.depth_points=display_look->depth_points;
        look.ambient=.8f;look.sky={.7f,.75f,.8f};look.ground={.4f,.45f,.5f};
        Target world=t;world.slot+=scene_descriptor_slots;
        // The screen binding is optional every frame; returning to idle must
        // restore the model's own material rather than sampling a stale game.
        gpu().views_made=0;
        return record_scene(world,&look,std::move(cabinet),&*display_look);
    } catch (...) {say_failed("the world display could not be drawn.");return false;}
}

void release() noexcept {
    {auto& s=shared();std::lock_guard lock(s.mutex);s.render_ready=false;s.prepared_ready=false;}
    surface().colour.Reset();surface().rtv.Reset();
    for (auto* context : {&gpu(),&prepared_gpu(),&display_gpu()}) {
    auto& g = *context;
    for (auto& texture:g.planar) texture.Reset();g.planar_rtvs.Reset();
    g.capture_depth.Reset();g.capture_depth_view.Reset();g.capture_width=0;g.capture_height=0;
    for (auto& pipeline:g.capture_pipelines) pipeline.Reset();g.capture_background.Reset();g.capture_format=DXGI_FORMAT_UNKNOWN;
    // The overlay waits for the GPU before it tears down, so nothing here is still in use.
    g.retired.clear();
    { auto& scene = scene_depth(); std::lock_guard lock(scene.mutex); scene.texture.Reset(); scene.wanted = scene.valid = scene.copied = false; }
    for (auto& pipeline : g.pipelines) pipeline.Reset();
    g.inset_background.Reset();
    g.pipeline_format = DXGI_FORMAT_UNKNOWN;
    g.root.Reset();
    g.model.reset();
    g.vertices.Reset();
    g.indices.Reset();
    g.textures.clear();
    g.next_texture = 0;
    g.views_made = 0;
    g.view_heap = nullptr;
    g.view_slot = ~0u;
    g.depth.Reset();
    g.depth_view.Reset();
    g.depth_width = 0;
    g.depth_height = 0;
    if (g.constants) g.constants->Unmap(0, nullptr);
    g.constants.Reset();
    g.constants_mapped = nullptr;
    g.constants_fence = {};
    g.constants_next = 0;
    g.upload_fence=0;
    }
}
} // namespace dingosdk::viewmodel
