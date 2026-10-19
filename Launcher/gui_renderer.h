#pragma once

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <imgui.h>

#include <array>
#include <filesystem>
#include <vector>

// The launcher window's Direct3D 12 renderer and its image decoding.
namespace dingosdk::launcher_gui::detail {

using Microsoft::WRL::ComPtr;

constexpr UINT frame_count = 2;

class Renderer {
public:
    bool init(HWND window);
    void render();
    // The swap chain's buffers at the window's new client size, once the GPU is done with the old ones.
    void resize(UINT width, UINT height);
    // Uploads RGBA pixels into a free SRV slot (slot 0 is the font) and
    // returns its ImGui texture id; empty when the slots are used up.
    ImTextureID upload_texture(const std::vector<unsigned char>& pixels, UINT width, UINT height);
    // Frees a texture's slot once the GPU is done with it (Mods browser icons).
    void release_texture(ImTextureID id);
    void shutdown();

private:
    struct Frame { ComPtr<ID3D12CommandAllocator> allocator; UINT64 fence_value{}; };
    struct Target { ComPtr<ID3D12Resource> resource; D3D12_CPU_DESCRIPTOR_HANDLE handle{}; };
    ComPtr<ID3D12Device> device_;
    // The background photo, tile icons and Thunderstore package icons.
    static constexpr UINT max_textures = 160;
    std::vector<ComPtr<ID3D12Resource>> textures_;   // index + 1 = SRV slot; null when free
    ComPtr<ID3D12DescriptorHeap> rtv_heap_, srv_heap_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<ID3D12GraphicsCommandList> list_;
    ComPtr<ID3D12Fence> fence_;
    ComPtr<IDXGISwapChain3> swap_;
    std::array<Frame, frame_count> frames_;
    std::array<Target, frame_count> targets_;
    HANDLE fence_event_{};
    HANDLE waitable_{};
    UINT64 fence_value_{};
    UINT64 frame_index_{};

    void wait(UINT64 value);
};

// Decodes a JPEG/PNG with WIC, scaled down to cover `cover` pixels at most.
bool decode_image(const std::vector<unsigned char>& bytes, ImVec2 cover, std::vector<unsigned char>& pixels,
                  UINT& width, UINT& height);
// ReSkateLauncher.background.jpg/.png beside the exe, else the embedded photo.
std::vector<unsigned char> background_bytes(const std::filesystem::path& directory);
// An embedded RCDATA resource; empty when it is missing.
std::vector<unsigned char> resource_bytes(const wchar_t* name);

} // namespace dingosdk::launcher_gui::detail
