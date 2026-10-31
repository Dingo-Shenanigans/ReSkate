#include "viewmodel.h"
#include "viewmodel_hlsl.h" // generated from viewmodel.hlsl (cmake/UI.cmake)
#include "Engine/Core/Log/logging.h"
#include <Windows.h>
#include <d3dcompiler.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <numbers>
#include <thread>

namespace dingosdk::viewmodel {
namespace {
using Microsoft::WRL::ComPtr;

constexpr std::uintmax_t most_file_bytes = 64ull * 1024 * 1024, most_picture_bytes = 64ull * 1024 * 1024;
constexpr UINT most_picture_side = 2048; // larger pictures are scaled down to this as they are read
constexpr float near_plane = 0.01f, far_plane = 50.0f;
// The frame's constants (viewmodel.hlsl's cbuffer frame): nine float4s, then the bones' rows.
constexpr std::size_t frame_header_floats = 9 * 4, frame_floats = frame_header_floats + max_bones * 12;
constexpr std::size_t constants_stride = (frame_floats * 4 + 255) / 256 * 256; // root CBVs are 256-byte aligned
constexpr std::size_t constants_ring = 8;

double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// A picture as the GPU takes it: RGBA8, its mip levels one after another, largest first.
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

struct Shared {
    std::mutex mutex;
    std::uint64_t serial{}; // which load is wanted; a reading that finishes for an older one is dropped
    Status status{Status::none};
    std::string why;
    std::shared_ptr<const Loaded> loaded;
    Look look;
    double until{};
    Animator animator;
};
Shared& shared() {
    static auto* value = new Shared;
    return *value;
}

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

void read_in_background(std::filesystem::path file, std::uint64_t serial) {
    auto loaded = std::make_shared<Loaded>();
    std::string why;
    std::string bytes;
    bool ok = read_file(file, most_file_bytes, bytes, why);
    if (ok) {
        auto model = parse(bytes, why);
        ok = model.has_value();
        if (ok) loaded->model = std::move(*model);
    }
    if (ok) {
        loaded->pictures.resize(loaded->model.materials.size() * 3);
        for (std::size_t m = 0; ok && m < loaded->model.materials.size(); ++m) {
            const auto& material = loaded->model.materials[m];
            const std::array<const std::string*, 3> names{&material.albedo, &material.normal, &material.surface};
            for (std::size_t k = 0; k < 3; ++k) {
                if (names[k]->empty()) continue;
                if (!plain_name(*names[k])) {
                    why = "material \"" + material.name + "\" names a picture that is not a .png or .jpg beside the model";
                    ok = false;
                    break;
                }
                std::string picture_bytes;
                auto& picture = loaded->pictures[m * 3 + k];
                if (!read_file(file.parent_path() / *names[k], most_picture_bytes, picture_bytes, why)) {
                    ok = false;
                    break;
                }
                if (!decode(picture_bytes, picture)) {
                    why = "\"" + *names[k] + "\" is not a PNG or JPG that can be read";
                    ok = false;
                    break;
                }
                picture.srgb = k == 0;
                make_levels(picture);
            }
        }
    }
    auto& s = shared();
    std::lock_guard lock(s.mutex);
    if (s.serial != serial) return;
    if (!ok) {
        s.status = Status::failed;
        s.why = why;
        logging::write(logging::Level::warning, logging::Channel::graphics, "Viewmodel " + file.filename().string() + " not loaded: " + why);
        return;
    }
    s.status = Status::ready;
    s.why.clear();
    s.loaded = std::move(loaded);
    s.animator.stop();
}

// ---- drawing -----------------------------------------------------------------------------------------
struct Retired {
    ComPtr<ID3D12Resource> resource;
    std::uint64_t fence{};
};
struct Gpu {
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
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
};
Gpu& gpu() {
    static auto* value = new Gpu;
    return *value;
}

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
    std::array<D3D12_ROOT_PARAMETER, 3> parameters{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants.ShaderRegister = 0;
    parameters[0].Constants.Num32BitValues = 4;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[1].Descriptor.ShaderRegister = 1;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[2].DescriptorTable.NumDescriptorRanges = 1;
    parameters[2].DescriptorTable.pDescriptorRanges = &range;
    parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_ANISOTROPIC;
    sampler.MaxAnisotropy = 8;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC description{};
    description.NumParameters = static_cast<UINT>(parameters.size());
    description.pParameters = parameters.data();
    description.NumStaticSamplers = 1;
    description.pStaticSamplers = &sampler;
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
    if (g.pipeline && g.pipeline_format == format) return true;
    g.pipeline.Reset();
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
    const std::array<D3D12_INPUT_ELEMENT_DESC, 6> layout{{
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Vertex, position), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Vertex, normal), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TANGENT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(Vertex, tangent), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(Vertex, uv), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"BLENDINDICES", 0, DXGI_FORMAT_R8G8B8A8_UINT, 0, offsetof(Vertex, joints), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"BLENDWEIGHT", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, offsetof(Vertex, weights), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
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
    description.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    description.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    description.NumRenderTargets = 1;
    description.RTVFormats[0] = format;
    description.SampleDesc.Count = 1;
    if (FAILED(device->CreateGraphicsPipelineState(&description, IID_PPV_ARGS(&g.pipeline)))) {
        say_failed("its pipeline was refused for this swapchain format.");
        return false;
    }
    g.pipeline_format = format;
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
    clear.DepthStencil.Depth = 1.0f;
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
        view.Format = i % 3 == 0 ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
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

std::array<float, 3> unit(std::array<float, 3> v, std::array<float, 3> fallback) {
    const float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (!(length > 1e-5f) || !std::isfinite(length)) return fallback;
    return {v[0] / length, v[1] / length, v[2] / length};
}
} // namespace

void load(const std::filesystem::path& file) {
    auto& s = shared();
    std::uint64_t serial{};
    {
        std::lock_guard lock(s.mutex);
        serial = ++s.serial;
        s.status = Status::loading;
        s.why.clear();
        s.loaded.reset();
        s.animator.stop();
    }
    std::thread(read_in_background, file, serial).detach();
}

void unload() {
    auto& s = shared();
    std::lock_guard lock(s.mutex);
    ++s.serial;
    s.status = Status::none;
    s.why.clear();
    s.loaded.reset();
    s.until = 0;
    s.animator.stop();
}

Info info() {
    auto& s = shared();
    std::lock_guard lock(s.mutex);
    Info out;
    out.status = s.status;
    out.why = s.why;
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
    s.until = now() + 0.3;
}
void hide() noexcept {
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
    s.animator.play(s.loaded->model, asked, now());
    return {};
}
void stop() noexcept {
    auto& s = shared();
    std::lock_guard lock(s.mutex);
    s.animator.stop();
}

State state() {
    auto& s = shared();
    std::lock_guard lock(s.mutex);
    if (!s.loaded) return {};
    const auto now_state = s.animator.state(s.loaded->model, now());
    if (now_state.clip < 0) return {};
    const auto& clip = s.loaded->model.clips[static_cast<std::size_t>(now_state.clip)];
    return {clip.name, now_state.time, clip.duration(), now_state.done};
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
    animator.pose(loaded->model, now(), std::span(skin).first(loaded->model.bones.size()));
    const auto pose = multiply(placement(look.offset, look.yaw, look.pitch, look.roll),
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
    auto& s = shared();
    std::lock_guard lock(s.mutex);
    if (gpu().failed || !s.loaded) return false;
    return now() < s.until || gpu().model != s.loaded || gpu().next_texture < gpu().textures.size();
}

bool record(const Target& t) noexcept {
    try {
        auto& g = gpu();
        if (g.failed || !t.device || !t.commands || !t.buffer || !t.srvs) return false;
        std::shared_ptr<const Loaded> loaded;
        Animator animator;
        Look look;
        bool shown{};
        const double at = now();
        {
            auto& s = shared();
            std::lock_guard lock(s.mutex);
            loaded = s.loaded;
            animator = s.animator;
            look = s.look;
            shown = at < s.until;
        }
        free_retired(t);
        if (g.model != loaded && !take_model(t, loaded)) return false;
        if (!g.model) return false;
        // Its pictures go up one a frame (each is up to 2048 pixels a side with its levels), and it is
        // drawn once they all have.
        if (g.next_texture < g.textures.size() && !upload_next_texture(t)) return false;
        if (!shown || g.model->model.materials.size() * 3 > t.slots) return false;
        make_views(t);
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
        const float aspect = static_cast<float>(desc.Width) / static_cast<float>(desc.Height);
        const float fov = std::clamp(look.fov, 20.0f, 120.0f) * std::numbers::pi_v<float> / 180.0f;
        const float focal = 1.0f / std::tan(fov * 0.5f);
        const float depth_scale = far_plane / (far_plane - near_plane);
        const auto place = placement(look.offset, look.yaw, look.pitch, look.roll);
        const auto light = unit(look.light, {-0.35f, 0.75f, -0.55f});
        const std::array<float, frame_header_floats> header{
            focal / aspect, focal, depth_scale, -near_plane * depth_scale,
            place[0], place[1], place[2], place[3], place[4], place[5], place[6], place[7], place[8], place[9], place[10], place[11],
            light[0], light[1], light[2], 0,
            look.light_colour[0], look.light_colour[1], look.light_colour[2], std::clamp(look.ambient, 0.0f, 4.0f),
            look.sky[0], look.sky[1], look.sky[2], std::clamp(look.brightness, 0.0f, 4.0f),
            look.ground[0], look.ground[1], look.ground[2], 0,
            static_cast<float>(t.transfer), t.white_nits, 0, 0};
        std::copy(header.begin(), header.end(), frame.begin());
        std::array<Affine, max_bones> skin;
        animator.pose(model, at, std::span(skin).first(model.bones.size()));
        for (std::size_t i = 0; i < model.bones.size(); ++i) std::copy(skin[i].begin(), skin[i].end(), frame.begin() + frame_header_floats + i * 12);
        std::memcpy(g.constants_mapped + ring * constants_stride, frame.data(), sizeof frame);

        transition(t.commands, t.buffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const auto depth = g.depth_view->GetCPUDescriptorHandleForHeapStart();
        t.commands->OMSetRenderTargets(1, &t.rtv, FALSE, &depth);
        t.commands->ClearDepthStencilView(depth, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(desc.Width), static_cast<float>(desc.Height), 0.0f, 1.0f};
        const D3D12_RECT scissor{0, 0, static_cast<LONG>(desc.Width), static_cast<LONG>(desc.Height)};
        t.commands->RSSetViewports(1, &viewport);
        t.commands->RSSetScissorRects(1, &scissor);
        t.commands->SetGraphicsRootSignature(g.root.Get());
        t.commands->SetPipelineState(g.pipeline.Get());
        t.commands->SetGraphicsRootConstantBufferView(1, g.constants->GetGPUVirtualAddress() + ring * constants_stride);
        t.commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        t.commands->IASetVertexBuffers(0, 1, &g.vertex_view);
        t.commands->IASetIndexBuffer(&g.index_view);
        const auto step = t.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        for (const auto& part : model.parts) {
            const auto& material = model.materials[part.material];
            const std::size_t first = std::size_t{part.material} * 3;
            const unsigned has = (g.textures[first] ? 1u : 0u) | (g.textures[first + 1] ? 2u : 0u) | (g.textures[first + 2] ? 4u : 0u);
            const std::array<float, 4> constants{material.roughness, material.specular, static_cast<float>(has), 0};
            t.commands->SetGraphicsRoot32BitConstants(0, 4, constants.data(), 0);
            auto table = t.srvs->GetGPUDescriptorHandleForHeapStart();
            table.ptr += static_cast<UINT64>(step) * (t.slot + first);
            t.commands->SetGraphicsRootDescriptorTable(2, table);
            t.commands->DrawIndexedInstanced(part.count, 1, part.first, 0, 0);
        }
        transition(t.commands, t.buffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        return true;
    } catch (...) {
        say_failed("an exception was raised while drawing it.");
        return false;
    }
}

void release() noexcept {
    auto& g = gpu();
    // The overlay waits for the GPU before it tears down, so nothing here is still in use.
    g.retired.clear();
    g.pipeline.Reset();
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
}
} // namespace dingosdk::viewmodel
