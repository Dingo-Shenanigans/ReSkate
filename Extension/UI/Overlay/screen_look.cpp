#include "screen_look.h"
#include "screen_look_hlsl.h" // generated from screen_look.hlsl (cmake/UI.cmake)
#include "Engine/Core/Log/logging.h"
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <mutex>
#include <numbers>
#include <optional>
#include <string>

// The graphics and the hand are the Filmer's (neekgate/ReSkate, filmer-zee, filmer_look.cpp); what to
// draw comes from a plugin (set) instead of the Filmer's settings.
namespace dingosdk::screen_look {
namespace {
using Microsoft::WRL::ComPtr;
// The shader's cbuffer: nine float4s, then five lines of twelve characters packed four to a uint.
constexpr std::size_t float_constants = 9 * 4, constant_count = float_constants + 16, text_lines = 5;
// The auto-iris's two 1x1 targets: x the brightness the iris settled to, y this frame's.
constexpr DXGI_FORMAT iris_format = DXGI_FORMAT_R32G32_FLOAT;
// When the hand reaches the glass, into its slap.
constexpr double slap_contact = 0.13;

// One full-screen triangle; the pixel shader does everything. Positions are in half the frame's
// height from its centre (q), so the lens maths reads the same at any resolution.
constexpr const char *shader_source = screen_look_hlsl;

double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// What a plugin asked for (client thread), read by the render thread.
struct Asked {
    std::mutex mutex;
    Settings settings;
    double until{}, rolling_origin{};
    double slap_at{-1e9};
    std::uint64_t slap_serial{};
};
Asked &asked() {
    static auto *value = new Asked;
    return *value;
}

struct Graphics {
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    DXGI_FORMAT pipeline_format{DXGI_FORMAT_UNKNOWN};
    ComPtr<ID3D12Resource> copy;
    D3D12_RESOURCE_DESC copy_desc{};
    D3D12_RESOURCE_STATES copy_state{D3D12_RESOURCE_STATE_COPY_DEST};
    ID3D12DescriptorHeap *view_heap{};
    unsigned view_slot{~0u};
    // The auto-iris: a pipeline of its own and two 1x1 targets it ping-pongs between, with their render
    // target views (a heap of its own) and shader views (the two slots after the copy's).
    ComPtr<ID3D12PipelineState> iris_pipeline;
    std::array<ComPtr<ID3D12Resource>, 2> iris;
    std::array<D3D12_RESOURCE_STATES, 2> iris_state{};
    ComPtr<ID3D12DescriptorHeap> iris_rtvs;
    ID3D12DescriptorHeap *iris_view_heap{};
    unsigned iris_index{};
    bool iris_cleared{};
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
    logging::write(logging::Level::warning, logging::Channel::graphics, "Screen look unavailable: " + why + " Plugins asking for it get the game's own picture.");
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

bool build_root(ID3D12Device *device) {
    auto &g = graphics();
    if (g.root) return true;
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    range.OffsetInDescriptorsFromTableStart = 0;
    D3D12_DESCRIPTOR_RANGE iris_range = range;
    iris_range.BaseShaderRegister = 1; // t1: the auto-iris
    std::array<D3D12_ROOT_PARAMETER, 3> parameters{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants.Num32BitValues = static_cast<UINT>(constant_count);
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable.NumDescriptorRanges = 1;
    parameters[1].DescriptorTable.pDescriptorRanges = &range;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[2] = parameters[1];
    parameters[2].DescriptorTable.pDescriptorRanges = &iris_range;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
    sampler.MaxLOD = 0.0f;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC description{};
    description.NumParameters = static_cast<UINT>(parameters.size());
    description.pParameters = parameters.data();
    description.NumStaticSamplers = 1;
    description.pStaticSamplers = &sampler;
    description.Flags = D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS | D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
                        D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;
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

bool build_pipeline(ID3D12Device *device, DXGI_FORMAT format) {
    auto &g = graphics();
    if (g.pipeline && g.iris_pipeline && g.pipeline_format == format) return true;
    g.pipeline.Reset();
    g.iris_pipeline.Reset();
    if (!build_root(device)) return false;
    ComPtr<ID3DBlob> vertex, pixel, iris_pixel, error;
    const auto length = std::strlen(shader_source);
    if (FAILED(D3DCompile(shader_source, length, "screen_look", nullptr, nullptr, "vs_main", "vs_5_0", 0, 0, &vertex, &error)) || !vertex) {
        say_failed(std::string("its vertex shader did not compile") +
                   (error ? std::string(": ") + static_cast<const char *>(error->GetBufferPointer()) : std::string(".")));
        return false;
    }
    error.Reset();
    if (FAILED(D3DCompile(shader_source, length, "screen_look", nullptr, nullptr, "ps_main", "ps_5_0", 0, 0, &pixel, &error)) || !pixel) {
        say_failed(std::string("its pixel shader did not compile") +
                   (error ? std::string(": ") + static_cast<const char *>(error->GetBufferPointer()) : std::string(".")));
        return false;
    }
    error.Reset();
    if (FAILED(D3DCompile(shader_source, length, "screen_look", nullptr, nullptr, "ps_adapt", "ps_5_0", 0, 0, &iris_pixel, &error)) || !iris_pixel) {
        say_failed(std::string("its auto-iris shader did not compile") +
                   (error ? std::string(": ") + static_cast<const char *>(error->GetBufferPointer()) : std::string(".")));
        return false;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
    description.pRootSignature = g.root.Get();
    description.VS = {vertex->GetBufferPointer(), vertex->GetBufferSize()};
    description.PS = {pixel->GetBufferPointer(), pixel->GetBufferSize()};
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
    description.NodeMask = 0;
    if (FAILED(device->CreateGraphicsPipelineState(&description, IID_PPV_ARGS(&g.pipeline)))) {
        say_failed("its pipeline was refused for this swapchain format.");
        return false;
    }
    description.PS = {iris_pixel->GetBufferPointer(), iris_pixel->GetBufferSize()};
    description.RTVFormats[0] = iris_format;
    if (FAILED(device->CreateGraphicsPipelineState(&description, IID_PPV_ARGS(&g.iris_pipeline)))) {
        say_failed("its auto-iris pipeline was refused.");
        return false;
    }
    g.pipeline_format = format;
    return true;
}

D3D12_CPU_DESCRIPTOR_HANDLE iris_rtv(unsigned index, ID3D12Device *device) {
    auto handle = graphics().iris_rtvs->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV)) * index;
    return handle;
}

// The auto-iris's targets, made once and cleared to "nothing measured yet" on their first use; their
// shader views go in the overlay's heap after the copy's (again when the overlay rebuilt it).
bool ensure_iris(const Target &t) {
    auto &g = graphics();
    if (!g.iris[0]) {
        D3D12_DESCRIPTOR_HEAP_DESC heap_description{};
        heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap_description.NumDescriptors = 2;
        if (FAILED(t.device->CreateDescriptorHeap(&heap_description, IID_PPV_ARGS(&g.iris_rtvs)))) {
            say_failed("the auto-iris's views could not be made.");
            return false;
        }
        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        description.Width = 1;
        description.Height = 1;
        description.DepthOrArraySize = 1;
        description.MipLevels = 1;
        description.Format = iris_format;
        description.SampleDesc.Count = 1;
        description.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_CLEAR_VALUE clear{};
        clear.Format = iris_format;
        for (unsigned i = 0; i < 2; ++i) {
            if (FAILED(t.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_RENDER_TARGET, &clear,
                                                         IID_PPV_ARGS(&g.iris[i])))) {
                g.iris = {};
                say_failed("the auto-iris's targets could not be made.");
                return false;
            }
            g.iris[i]->SetName(L"ReSkate screen look auto-iris");
            g.iris_state[i] = D3D12_RESOURCE_STATE_RENDER_TARGET;
            t.device->CreateRenderTargetView(g.iris[i].Get(), nullptr, iris_rtv(i, t.device));
        }
        g.iris_cleared = false;
        g.iris_view_heap = nullptr;
    }
    if (!g.iris_cleared) {
        const std::array<float, 4> zero{};
        for (unsigned i = 0; i < 2; ++i) {
            transition(t.commands, g.iris[i].Get(), g.iris_state[i], D3D12_RESOURCE_STATE_RENDER_TARGET);
            t.commands->ClearRenderTargetView(iris_rtv(i, t.device), zero.data(), 0, nullptr);
            transition(t.commands, g.iris[i].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            g.iris_state[i] = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        }
        g.iris_cleared = true;
    }
    if (g.iris_view_heap != t.srvs) {
        const auto step = t.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        for (unsigned i = 0; i < 2; ++i) {
            auto cpu = t.srvs->GetCPUDescriptorHandleForHeapStart();
            cpu.ptr += static_cast<SIZE_T>(step) * (t.slot + 1 + i);
            D3D12_SHADER_RESOURCE_VIEW_DESC view{};
            view.Format = iris_format;
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            view.Texture2D.MipLevels = 1;
            t.device->CreateShaderResourceView(g.iris[i].Get(), &view, cpu);
        }
        g.iris_view_heap = t.srvs;
    }
    return true;
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
    const bool same = g.copy && g.copy_desc.Width == wanted.Width && g.copy_desc.Height == wanted.Height &&
                      g.copy_desc.Format == wanted.Format;
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
        g.copy->SetName(L"ReSkate screen look frame copy");
        g.copy_desc = wanted;
        g.copy_state = D3D12_RESOURCE_STATE_COPY_DEST;
        g.view_heap = nullptr;
    }
    // Its view, in the overlay's heap (a new heap after the overlay rebuilt its graphics).
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

// The hand slapping the lens, moved along a path in front of it (metres: x right, y down, z away from
// the glass) and seen in perspective: small and off in a lower corner where the free arm reaches round
// the camera, growing fast as it accelerates onto the glass; pressed there a beat, sliding a little;
// pulled away.
struct Hand { float x{}, y{}, angle{}, size{}, blur{}; bool mirrored{}; };
struct Key { double time; float x, y, z, angle; };
constexpr std::array<Key, 4> slap_keys{{{0.0, 0.55f, 0.45f, 0.36f, -0.62f}, {slap_contact, 0.015f, 0.010f, 0.050f, -0.45f},
                                       {0.25, 0.023f, 0.016f, 0.052f, -0.43f}, {0.60, 0.62f, 0.55f, 0.40f, -0.70f}}};
constexpr float hand_scale = 0.225f; // size at 1 m (it fills the frame from about 3: 7.5 cm off the glass)
float smooth(float u) { u = std::clamp(u, 0.0f, 1.0f); return u * u * (3.0f - 2.0f * u); }
std::optional<Key> slap_key(double since) {
    const auto &keys = slap_keys;
    if (since < 0.0 || since > keys.back().time) return std::nullopt;
    for (std::size_t i = 0; i + 1 < keys.size(); ++i) {
        const auto &a = keys[i], &b = keys[i + 1];
        if (since > b.time) continue;
        float u = static_cast<float>((since - a.time) / (b.time - a.time));
        if (i == 0) u = std::pow(u, 2.2f); // accelerating onto the glass
        else if (i != 1) u = smooth(u);    // (pressed: a steady slide)
        return Key{since, a.x + (b.x - a.x) * u, a.y + (b.y - a.y) * u, a.z + (b.z - a.z) * u, a.angle + (b.angle - a.angle) * u};
    }
    return std::nullopt;
}
Hand hand_at(double since, std::uint64_t serial) {
    Hand hand;
    const auto key = slap_key(since);
    if (!key) return hand;
    const auto next = slap_key(since + 0.01).value_or(*key);
    hand.mirrored = (serial & 1u) != 0;
    const float side = hand.mirrored ? -1.0f : 1.0f;
    constexpr float lens = 0.84f; // half heights per unit of x / z
    hand.x = side * key->x / key->z * lens;
    hand.y = key->y / key->z * lens;
    hand.angle = key->angle;
    hand.size = hand_scale / key->z;
    const float growth = std::abs(hand_scale / next.z - hand.size) / 0.01f; // motion blur from how fast it moves
    hand.blur = 0.02f + std::min(growth / 40.0f, 1.0f) * 0.08f;
    return hand;
}

// The camcorder's iris: closing down as the hand covers the lens, overshooting as the palm comes off.
float exposure_at(double since) {
    const Hand hand = hand_at(since, 0);
    const float covered = std::clamp((hand.size - 2.0f) / 2.5f, 0.0f, 1.0f);
    float exposure = 1.0f - 0.4f * covered;
    if (since > 0.25 && since < 1.5) {
        const float t = static_cast<float>(since - 0.25);
        exposure += 0.3f * std::exp(-t * 4.0f) * std::min(t / 0.1f, 1.0f);
    }
    return exposure;
}

std::uint32_t bits(float value) { return std::bit_cast<std::uint32_t>(value); }
std::string right_aligned(std::string text) {
    if (text.size() < text_columns) text.insert(0, text_columns - text.size(), ' ');
    return text.substr(0, text_columns);
}
} // namespace

void set(const Settings &settings) noexcept {
    try {
        auto &a = asked();
        std::lock_guard lock(a.mutex);
        const double at = now();
        // The tape's roll is kept counting between ticks; a jump of more than a moment is a new roll.
        const double origin = settings.rolling >= 0 ? at - settings.rolling : -1;
        if (origin < 0 || a.rolling_origin < 0 || std::abs(origin - a.rolling_origin) > 0.25) a.rolling_origin = origin;
        a.settings = settings;
        a.until = at + 0.3;
    } catch (...) {}
}
void clear() noexcept {
    auto &a = asked();
    std::lock_guard lock(a.mutex);
    a.until = 0;
}
void slap() noexcept {
    auto &a = asked();
    std::lock_guard lock(a.mutex);
    const double at = now();
    if (at - a.slap_at < slap_keys.back().time) return; // one at a time
    a.slap_at = at;
    ++a.slap_serial;
}

bool pending() noexcept {
    auto &a = asked();
    std::lock_guard lock(a.mutex);
    return now() < a.until && !graphics().failed;
}

bool record(const Target &t) noexcept {
    try {
        auto &g = graphics();
        Settings look;
        double rolling_origin{}, slap_at{};
        std::uint64_t slap_serial{};
        {
            auto &a = asked();
            std::lock_guard lock(a.mutex);
            if (now() >= a.until) return false;
            look = a.settings;
            rolling_origin = a.rolling_origin;
            slap_at = a.slap_at;
            slap_serial = a.slap_serial;
        }
        if (g.failed || !t.device || !t.commands || !t.buffer || !t.srvs) return false;
        if (!build_pipeline(t.device, t.format) || !ensure_copy(t) || !ensure_iris(t)) return false;
        const auto desc = t.buffer->GetDesc();
        const float width = static_cast<float>(desc.Width), height = static_cast<float>(desc.Height);
        if (width < 16.0f || height < 16.0f) return false;
        // Copy the frame, then draw it back through the shader.
        transition(t.commands, t.buffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
        transition(t.commands, g.copy.Get(), g.copy_state, D3D12_RESOURCE_STATE_COPY_DEST);
        t.commands->CopyResource(g.copy.Get(), t.buffer);
        transition(t.commands, g.copy.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        g.copy_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        transition(t.commands, t.buffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const double seconds = now();
        const float frame_seconds = static_cast<float>(std::clamp(seconds - g.last_seconds, 0.0, 0.25));
        g.last_seconds = seconds;
        const double since = seconds - slap_at;
        const Hand hand = hand_at(since, slap_serial);
        const float fov = std::clamp(look.fov, 5.0f, 170.0f) * std::numbers::pi_v<float> / 180.0f;
        const float screen_aspect = width / height;
        // Text white: the overlay's output (SDR, scRGB in units of 80 nits, or HDR10's PQ code for 200 nits).
        const float white = t.transfer == 1 ? 2.5f : t.transfer == 2 ? 0.58f : 0.95f;
        std::array<std::string, text_lines> lines = look.text;
        for (const std::size_t right : {std::size_t{1}, std::size_t{3}, std::size_t{4}})
            if (!lines[right].empty()) lines[right] = right_aligned(lines[right]);
        const bool any_text = std::ranges::any_of(lines, [](const std::string &line) { return !line.empty(); });
        std::array<std::uint32_t, constant_count> constants{
            bits(std::clamp(look.bend, 0.0f, 1.0f)), bits(fov * 0.5f), bits(std::tan(fov * 0.5f)), bits(look.bars ? 4.0f / 3.0f : screen_aspect),
            bits(screen_aspect), bits(look.bars ? 1.0f : 0.0f), bits(look.hood ? 1.0f : 0.0f), bits(static_cast<float>(std::fmod(seconds, 1000.0))),
            bits(look.grade ? 1.0f : 0.0f), bits(look.scanlines ? 1.0f : 0.0f), bits(look.grain ? 1.0f : 0.0f), bits(look.soft ? 1.0f : 0.0f),
            bits(static_cast<float>(t.transfer)), bits(exposure_at(since)), bits(width), bits(height),
            bits(hand.x), bits(hand.y), bits(hand.angle), bits(hand.size),
            bits(hand.mirrored ? 1.0f : 0.0f), bits(hand.blur), bits(0.0f), bits(0.0f),
            bits(2.0f / 150.0f), bits(white), bits(any_text ? 1.0f : 0.0f), bits(0.0f),
            bits(std::clamp(look.age, 0.0f, 1.0f)), bits(look.dropouts ? 1.0f : 0.0f), bits(look.head_switching ? 1.0f : 0.0f), bits(look.smear ? 1.0f : 0.0f),
            bits(rolling_origin >= 0 ? static_cast<float>(seconds - rolling_origin) : -1.0f), bits(look.tracking ? 1.0f : 0.0f),
            bits(look.iris ? 1.0f : 0.0f), bits(frame_seconds)};
        for (std::size_t line = 0; line < text_lines; ++line)
            for (std::size_t column = 0; column < text_columns && column < lines[line].size(); ++column) {
                const auto code = static_cast<unsigned char>(std::toupper(static_cast<unsigned char>(lines[line][column])));
                const std::size_t slot = float_constants + line * 3 + column / 4;
                constants[slot] |= std::uint32_t{code} << (8 * (column % 4));
            }
        const auto step = t.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        const auto view_at = [&](unsigned slot) {
            auto gpu = t.srvs->GetGPUDescriptorHandleForHeapStart();
            gpu.ptr += static_cast<UINT64>(step) * slot;
            return gpu;
        };
        t.commands->SetGraphicsRootSignature(g.root.Get());
        t.commands->SetGraphicsRoot32BitConstants(0, static_cast<UINT>(constants.size()), constants.data(), 0);
        t.commands->SetGraphicsRootDescriptorTable(1, view_at(t.slot));
        t.commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        // The auto-iris: measure this frame and follow it, into one target from the other.
        const unsigned current = g.iris_index ^ 1u, previous = g.iris_index;
        if (look.iris) {
            g.iris_index = current;
            transition(t.commands, g.iris[current].Get(), g.iris_state[current], D3D12_RESOURCE_STATE_RENDER_TARGET);
            const auto target = iris_rtv(current, t.device);
            t.commands->OMSetRenderTargets(1, &target, FALSE, nullptr);
            const D3D12_VIEWPORT one{0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f};
            const D3D12_RECT one_pixel{0, 0, 1, 1};
            t.commands->RSSetViewports(1, &one);
            t.commands->RSSetScissorRects(1, &one_pixel);
            t.commands->SetPipelineState(g.iris_pipeline.Get());
            t.commands->SetGraphicsRootDescriptorTable(2, view_at(t.slot + 1 + previous));
            t.commands->DrawInstanced(3, 1, 0, 0);
            transition(t.commands, g.iris[current].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            g.iris_state[current] = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        }
        // The picture.
        t.commands->OMSetRenderTargets(1, &t.rtv, FALSE, nullptr);
        const D3D12_VIEWPORT viewport{0.0f, 0.0f, width, height, 0.0f, 1.0f};
        const D3D12_RECT scissor{0, 0, static_cast<LONG>(desc.Width), static_cast<LONG>(desc.Height)};
        t.commands->RSSetViewports(1, &viewport);
        t.commands->RSSetScissorRects(1, &scissor);
        t.commands->SetPipelineState(g.pipeline.Get());
        t.commands->SetGraphicsRootDescriptorTable(2, view_at(t.slot + 1 + g.iris_index));
        t.commands->DrawInstanced(3, 1, 0, 0);
        // The overlay carries on from PRESENT, as without the look.
        transition(t.commands, t.buffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        return true;
    } catch (...) {
        say_failed("an exception was raised while drawing it.");
        return false;
    }
}

void release() noexcept {
    auto &g = graphics();
    g.copy.Reset();
    g.copy_desc = {};
    g.copy_state = D3D12_RESOURCE_STATE_COPY_DEST;
    g.pipeline.Reset();
    g.pipeline_format = DXGI_FORMAT_UNKNOWN;
    g.root.Reset();
    g.view_heap = nullptr;
    g.view_slot = ~0u;
    g.iris_pipeline.Reset();
    g.iris = {};
    g.iris_rtvs.Reset();
    g.iris_view_heap = nullptr;
    g.iris_cleared = false;
}
} // namespace dingosdk::screen_look
