#include "screen_shader.h"
#include "screen_shader_vs_dxil.h"
#include "plugin_shader_compile.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/clock.h"
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <map>
#include <mutex>
#include <thread>
#include <utility>

namespace dingosdk::screen_shader {
namespace {
using Microsoft::WRL::ComPtr;
// The cbuffer as root constants: ReSkate's two float4s, then the plugin's.
constexpr std::size_t constant_count = 8 + value_count;
// Frames timed on the GPU and not yet read back, each its own pair of timestamps.
constexpr std::size_t timing_ring = 8;

// One full-screen triangle (the DXIL build of it is screen_shader_vs_dxil.h).
constexpr const char *vertex_source = R"hlsl(
struct Interpolated { float4 position : SV_Position; float2 uv : TEXCOORD0; };
Interpolated main(uint id : SV_VertexID) {
    Interpolated o;
    const float2 p = float2((id << 1) & 2, id & 2);
    o.position = float4(p * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    o.uv = p;
    return o;
}
)hlsl";

// A shader is known by its file and content (FNV-1a over both).
std::uint64_t key_of(std::string_view name, std::string_view bytes) {
    std::uint64_t hash = 14695981039346656037ull;
    const auto mix = [&](std::string_view part) {
        for (const unsigned char c : part) hash = (hash ^ c) * 1099511628211ull;
        hash = (hash ^ 0xffu) * 1099511628211ull;
    };
    mix(name);
    mix(bytes);
    return hash;
}

// A shader's code: compiled (HLSL, on a thread of its own: it can take seconds) or as given.
struct Program {
    enum class State : std::uint8_t { compiling, ready, refused } state{State::compiling};
    std::string code;   // DXBC or DXIL
    bool dxil{};
    std::string why;    // refused: why (said to the plugin, and once in the log)
};

// What a plugin asked for (client thread), and every shader's program (any thread), under the mutex.
struct Asked {
    std::mutex mutex;
    std::string owner, name;
    std::shared_ptr<const std::string> bytes;
    Kind kind{Kind::hlsl};
    std::uint64_t key{};
    std::array<float, value_count> values{};
    double until{};
    std::map<std::uint64_t, std::shared_ptr<Program>> programs;
};
Asked &asked() {
    static auto *value = new Asked;
    return *value;
}

// Refuses a shader for the rest of the run, and says so once in the log.
void refuse(Program &program, std::string_view name, std::string why) {
    if (program.state == Program::State::refused) return;
    program.state = Program::State::refused;
    program.why = std::move(why);
    logging::write(logging::Level::warning, logging::Channel::graphics,
                   std::format("Screen shader {} is off for this run: {}.", name, program.why));
}

struct Graphics {
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3DBlob> vertex_dxbc;
    std::map<std::pair<std::uint64_t, DXGI_FORMAT>, ComPtr<ID3D12PipelineState>> pipelines;
    ComPtr<ID3D12Resource> copy;
    D3D12_RESOURCE_DESC copy_desc{};
    D3D12_RESOURCE_STATES copy_state{D3D12_RESOURCE_STATE_COPY_DEST};
    ID3D12DescriptorHeap *view_heap{};
    unsigned view_slot{~0u};
    // The GPU's time for each frame drawn: a pair of timestamps a frame, resolved into the readback buffer, read
    // once the overlay's fence has passed that frame.
    ComPtr<ID3D12QueryHeap> queries;
    ComPtr<ID3D12Resource> readback;
    const std::uint64_t *times{};
    struct Timed { std::uint64_t fence{}, key{}; bool waiting{}; };
    std::array<Timed, timing_ring> timed{};
    std::size_t next_timed{};
    std::uint64_t timed_key{};   // the shader the sums below are for
    double timed_ms{};
    unsigned timed_frames{};
    unsigned timed_long{};       // its frames over shader_frame_most_ms in this stretch
    double last_seconds{};
    bool failed{};
};
Graphics &graphics() {
    static auto *value = new Graphics;
    return *value;
}

void say_failed(const std::string &why) {
    auto &g = graphics();
    if (g.failed) return;
    g.failed = true;
    logging::write(logging::Level::warning, logging::Channel::graphics,
                   "Screen shaders unavailable: " + why + " Plugins asking for one get the game's own picture.");
}

void transition(ID3D12GraphicsCommandList *list, ID3D12Resource *resource, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    if (from == to) return;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = from;
    barrier.Transition.StateAfter = to;
    list->ResourceBarrier(1, &barrier);
}

// HLSL compiled on a thread of its own (plugin_shader_compile.h: DXIL with the game's own compiler where it loads, else
// DXBC); the program is ready or refused when it is done.
void compile(std::shared_ptr<Program> program, std::string name, std::shared_ptr<const std::string> bytes) {
    std::thread([program = std::move(program), name = std::move(name), bytes = std::move(bytes)] {
        auto made = plugin_shader_compile::compile_pixel_shader(std::string(prelude) + *bytes, name);
        auto& a = asked();
        std::lock_guard lock(a.mutex);
        if (!made.code.empty()) {
            program->code = std::move(made.code);
            program->dxil = made.dxil;
            program->state = Program::State::ready;
        } else {
            refuse(*program, name, "it did not compile: " + made.error);
        }
    }).detach();
}

bool build_root(ID3D12Device *device) {
    auto &g = graphics();
    if (g.root) return true;
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    std::array<D3D12_ROOT_PARAMETER, 2> parameters{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants.Num32BitValues = static_cast<UINT>(constant_count);
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable.NumDescriptorRanges = 1;
    parameters[1].DescriptorTable.pDescriptorRanges = &range;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    std::array<D3D12_STATIC_SAMPLER_DESC, 2> samplers{};
    for (UINT i = 0; i < samplers.size(); ++i) {
        auto &sampler = samplers[i];
        sampler.Filter = i == 0 ? D3D12_FILTER_MIN_MAG_MIP_LINEAR : D3D12_FILTER_MIN_MAG_MIP_POINT;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
        sampler.MaxLOD = 0.0f;
        sampler.ShaderRegister = i;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    D3D12_ROOT_SIGNATURE_DESC description{};
    description.NumParameters = static_cast<UINT>(parameters.size());
    description.pParameters = parameters.data();
    description.NumStaticSamplers = static_cast<UINT>(samplers.size());
    description.pStaticSamplers = samplers.data();
    description.Flags = D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS | D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
                        D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;
    ComPtr<ID3DBlob> blob, error;
    if (FAILED(D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)) || !blob) {
        say_failed("their root signature could not be built.");
        return false;
    }
    if (FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g.root)))) {
        say_failed("their root signature was refused.");
        return false;
    }
    return true;
}

// The pipeline for a program and the back buffer's format: null when the driver refuses it (the program is
// refused then, under the caller's lock of asked()).
ID3D12PipelineState *pipeline_for(ID3D12Device *device, std::uint64_t key, Program &program, std::string_view name, DXGI_FORMAT format) {
    auto &g = graphics();
    if (const auto found = g.pipelines.find({key, format}); found != g.pipelines.end()) return found->second.Get();
    if (!build_root(device)) return nullptr;
    D3D12_SHADER_BYTECODE vertex{};
    if (program.dxil) {
        vertex = {vertex_dxil, sizeof(vertex_dxil)};
    } else {
        if (!g.vertex_dxbc) {
            ComPtr<ID3DBlob> error;
            if (FAILED(D3DCompile(vertex_source, std::strlen(vertex_source), "screen_shader", nullptr, nullptr, "main", "vs_5_0", 0, 0,
                                  &g.vertex_dxbc, &error)) || !g.vertex_dxbc) {
                say_failed("their vertex shader did not compile.");
                return nullptr;
            }
        }
        vertex = {g.vertex_dxbc->GetBufferPointer(), g.vertex_dxbc->GetBufferSize()};
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
    description.pRootSignature = g.root.Get();
    description.VS = vertex;
    description.PS = {program.code.data(), program.code.size()};
    description.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    description.SampleMask = UINT_MAX;
    description.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    description.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    description.RasterizerState.DepthClipEnable = TRUE;
    description.DepthStencilState.DepthEnable = FALSE;
    description.DepthStencilState.StencilEnable = FALSE;
    description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    description.NumRenderTargets = 1;
    description.RTVFormats[0] = format;
    description.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> pipeline;
    if (FAILED(device->CreateGraphicsPipelineState(&description, IID_PPV_ARGS(&pipeline))) || !pipeline) {
        refuse(program, name, "the driver refused it (its bindings, inputs or output are not a screen shader's, or its DXIL is not signed)");
        return nullptr;
    }
    return g.pipelines.emplace(std::pair{key, format}, std::move(pipeline)).first->second.Get();
}

// The copy of the frame the shader reads, the size and format of the back buffer.
bool ensure_copy(const Target &t) {
    auto &g = graphics();
    auto wanted = t.buffer->GetDesc();
    wanted.Flags = D3D12_RESOURCE_FLAG_NONE;
    wanted.Alignment = 0;
    wanted.MipLevels = 1;
    wanted.DepthOrArraySize = 1;
    wanted.SampleDesc = {1, 0};
    wanted.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    const bool same = g.copy && g.copy_desc.Width == wanted.Width && g.copy_desc.Height == wanted.Height && g.copy_desc.Format == wanted.Format;
    if (!same) {
        // Frames still on the GPU may read the copy this one replaces.
        if (g.copy && (!t.wait_idle || !t.wait_idle())) return false;
        g.copy.Reset();
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        if (FAILED(t.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &wanted, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                     IID_PPV_ARGS(&g.copy)))) {
            say_failed("the copy of the frame could not be made.");
            return false;
        }
        g.copy->SetName(L"ReSkate screen shader frame copy");
        g.copy_desc = wanted;
        g.copy_state = D3D12_RESOURCE_STATE_COPY_DEST;
        g.view_heap = nullptr;
    }
    if (g.view_heap != t.srvs || g.view_slot != t.slot) {
        const auto step = t.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        auto cpu = t.srvs->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += static_cast<SIZE_T>(step) * t.slot;
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = wanted.Format;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture2D.MipLevels = 1;
        t.device->CreateShaderResourceView(g.copy.Get(), &view, cpu);
        g.view_heap = t.srvs;
        g.view_slot = t.slot;
    }
    return true;
}

// The timestamps' heap and where they are read.
bool ensure_timing(const Target &t) {
    auto &g = graphics();
    if (g.queries) return true;
    D3D12_QUERY_HEAP_DESC heap_description{};
    heap_description.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    heap_description.Count = static_cast<UINT>(timing_ring * 2);
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bytes{};
    bytes.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bytes.Width = timing_ring * 2 * sizeof(std::uint64_t);
    bytes.Height = 1;
    bytes.DepthOrArraySize = 1;
    bytes.MipLevels = 1;
    bytes.SampleDesc.Count = 1;
    bytes.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    void *mapped{};
    if (FAILED(t.device->CreateQueryHeap(&heap_description, IID_PPV_ARGS(&g.queries))) ||
        FAILED(t.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bytes, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                 IID_PPV_ARGS(&g.readback))) ||
        FAILED(g.readback->Map(0, nullptr, &mapped)) || !mapped) {
        g.queries.Reset();
        g.readback.Reset();
        say_failed("the GPU's time for them cannot be measured, and they are not drawn unmeasured.");
        return false;
    }
    g.times = static_cast<const std::uint64_t *>(mapped);
    g.timed = {};
    return true;
}

// The frames the GPU has finished: their times, against the budget. Under asked()'s lock.
void read_times(const Target &t, Asked &a) {
    auto &g = graphics();
    const std::uint64_t done = t.gpu_done ? t.gpu_done() : 0;
    for (std::size_t i = 0; i < timing_ring; ++i) {
        auto &slot = g.timed[i];
        if (!slot.waiting || slot.fence > done) continue;
        slot.waiting = false;
        const std::uint64_t start = g.times[i * 2], end = g.times[i * 2 + 1];
        if (end <= start || !t.timestamp_frequency) continue;
        const double ms = static_cast<double>(end - start) * 1000.0 / static_cast<double>(t.timestamp_frequency);
        if (slot.key != g.timed_key) {
            g.timed_key = slot.key;
            g.timed_ms = 0;
            g.timed_frames = 0;
            g.timed_long = 0;
        }
        const auto found = a.programs.find(slot.key);
        if (found == a.programs.end()) continue;
        const std::string name = slot.key == a.key ? a.name : std::string("(an earlier one)");
        if (ms > shader_frame_most_ms) {
            // Not at the first: one frame can be long for a reason that is not the shader's (the game's window left and
            // come back to, another program on the GPU). A long frame is left out of the average, and counted.
            if (++g.timed_long >= shader_long_frames)
                refuse(*found->second, name, std::format("{} frames of it in {} took over {:.0f} ms on the GPU (the last {:.1f})", shader_long_frames,
                                                         shader_budget_frames, shader_frame_most_ms, ms));
            continue;
        }
        g.timed_ms += ms;
        if (++g.timed_frames >= shader_budget_frames) {
            const double average = g.timed_ms / g.timed_frames;
            g.timed_ms = 0;
            g.timed_frames = 0;
            g.timed_long = 0;
            if (average > shader_budget_ms)
                refuse(*found->second, name, std::format("it took {:.2f} ms a frame on the GPU (at most {:.1f})", average, shader_budget_ms));
        }
    }
}

std::uint32_t bits(float value) { return std::bit_cast<std::uint32_t>(value); }
} // namespace

std::string set(std::string_view owner, std::string_view name, std::shared_ptr<const std::string> bytes, Kind kind,
                const std::array<float, value_count> &values) noexcept {
    try {
        if (!bytes) return "it has no content";
        auto &a = asked();
        std::shared_ptr<Program> start; // an HLSL program to compile, outside the lock
        {
            std::lock_guard lock(a.mutex);
            const double at = steady_seconds();
            if (at < a.until && a.owner != owner) return "another plugin's screen shader is on";
            const std::uint64_t key = key_of(name, *bytes);
            auto &program = a.programs[key];
            if (!program) {
                program = std::make_shared<Program>();
                if (kind == Kind::hlsl) {
                    start = program;
                } else {
                    program->code = *bytes;
                    program->dxil = kind == Kind::dxil;
                    program->state = Program::State::ready;
                }
            }
            if (program->state == Program::State::refused) return program->why;
            a.owner = std::string(owner);
            a.name = std::string(name);
            a.bytes = bytes;
            a.kind = kind;
            a.key = key;
            for (std::size_t i = 0; i < value_count; ++i) a.values[i] = std::isfinite(values[i]) ? values[i] : 0.0f;
            a.until = at + 0.3;
        }
        if (start) compile(std::move(start), std::string(name), std::move(bytes));
        return {};
    } catch (...) {
        return "it could not be taken";
    }
}

void clear(std::string_view owner) noexcept {
    auto &a = asked();
    std::lock_guard lock(a.mutex);
    if (owner.empty() || a.owner == owner) a.until = 0;
}

bool pending() noexcept {
    auto &a = asked();
    std::lock_guard lock(a.mutex);
    if (steady_seconds() >= a.until || graphics().failed) return false;
    const auto found = a.programs.find(a.key);
    return found != a.programs.end() && found->second->state == Program::State::ready;
}

bool record(const Target &t) noexcept {
    try {
        auto &g = graphics();
        if (g.failed || !t.device || !t.commands || !t.buffer || !t.srvs || !t.gpu_done || !t.timestamp_frequency) return false;
        if (!ensure_timing(t)) return false;
        auto &a = asked();
        std::lock_guard lock(a.mutex);
        read_times(t, a);
        if (steady_seconds() >= a.until) return false;
        const auto found = a.programs.find(a.key);
        if (found == a.programs.end() || found->second->state != Program::State::ready) return false;
        // A slot whose frame the GPU has not timed yet is not drawn over: the frame goes without the shader.
        auto &slot = g.timed[g.next_timed];
        if (slot.waiting) return false;
        auto *pipeline = pipeline_for(t.device, a.key, *found->second, a.name, t.format);
        if (!pipeline || !ensure_copy(t)) return false;
        const auto desc = t.buffer->GetDesc();
        const float width = static_cast<float>(desc.Width), height = static_cast<float>(desc.Height);
        if (width < 16.0f || height < 16.0f) return false;
        const auto timed = static_cast<UINT>(g.next_timed);
        g.next_timed = (g.next_timed + 1) % timing_ring;
        slot = {t.fence, a.key, true};
        t.commands->EndQuery(g.queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, timed * 2);
        // Copy the frame, then draw it back through the shader.
        transition(t.commands, t.buffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
        transition(t.commands, g.copy.Get(), g.copy_state, D3D12_RESOURCE_STATE_COPY_DEST);
        t.commands->CopyResource(g.copy.Get(), t.buffer);
        transition(t.commands, g.copy.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        g.copy_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        transition(t.commands, t.buffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const double seconds = steady_seconds();
        const float frame_seconds = static_cast<float>(std::clamp(seconds - g.last_seconds, 0.0, 0.25));
        g.last_seconds = seconds;
        std::array<std::uint32_t, constant_count> constants{bits(width), bits(height), bits(1.0f / width), bits(1.0f / height),
                                                            bits(static_cast<float>(std::fmod(seconds, 1000.0))), bits(frame_seconds),
                                                            bits(static_cast<float>(t.transfer)), bits(t.white_nits)};
        for (std::size_t i = 0; i < value_count; ++i) constants[8 + i] = bits(a.values[i]);
        const auto step = t.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        auto view = t.srvs->GetGPUDescriptorHandleForHeapStart();
        view.ptr += static_cast<UINT64>(step) * t.slot;
        t.commands->SetGraphicsRootSignature(g.root.Get());
        t.commands->SetGraphicsRoot32BitConstants(0, static_cast<UINT>(constants.size()), constants.data(), 0);
        t.commands->SetGraphicsRootDescriptorTable(1, view);
        t.commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        t.commands->OMSetRenderTargets(1, &t.rtv, FALSE, nullptr);
        const D3D12_VIEWPORT viewport{0.0f, 0.0f, width, height, 0.0f, 1.0f};
        const D3D12_RECT scissor{0, 0, static_cast<LONG>(desc.Width), static_cast<LONG>(desc.Height)};
        t.commands->RSSetViewports(1, &viewport);
        t.commands->RSSetScissorRects(1, &scissor);
        t.commands->SetPipelineState(pipeline);
        t.commands->DrawInstanced(3, 1, 0, 0);
        t.commands->EndQuery(g.queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, timed * 2 + 1);
        t.commands->ResolveQueryData(g.queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, timed * 2, 2, g.readback.Get(),
                                     static_cast<UINT64>(timed) * 2 * sizeof(std::uint64_t));
        // The overlay carries on from PRESENT, as without the shader.
        transition(t.commands, t.buffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        return true;
    } catch (...) {
        say_failed("an exception was raised while drawing one.");
        return false;
    }
}

void release() noexcept {
    auto &g = graphics();
    g.copy.Reset();
    g.copy_desc = {};
    g.copy_state = D3D12_RESOURCE_STATE_COPY_DEST;
    g.pipelines.clear();
    g.root.Reset();
    g.view_heap = nullptr;
    g.view_slot = ~0u;
    if (g.readback) g.readback->Unmap(0, nullptr);
    g.times = nullptr;
    g.readback.Reset();
    g.queries.Reset();
    g.timed = {};
    g.next_timed = 0;
    g.timed_key = 0;
    g.timed_ms = 0;
    g.timed_frames = 0;
    g.timed_long = 0;
}
} // namespace dingosdk::screen_shader
