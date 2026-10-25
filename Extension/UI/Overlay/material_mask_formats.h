#pragma once
#include <d3d12.h>

namespace dingosdk::overlay::detail
{
inline D3D12_RT_FORMAT_ARRAY material_mask_formats()
{
    // D3D12 rejects non-UNKNOWN formats beyond NumRenderTargets. Native draws
    // can have five or more targets; the private mask ABI always has four.
    D3D12_RT_FORMAT_ARRAY result{};
    result.NumRenderTargets = 4;
    result.RTFormats[0] = DXGI_FORMAT_R32G32B32A32_FLOAT;
    result.RTFormats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    result.RTFormats[2] = DXGI_FORMAT_R8G8B8A8_UNORM;
    result.RTFormats[3] = DXGI_FORMAT_R32G32B32A32_UINT;
    return result;
}
// Position and calibration retain full precision. Normals/thickness use half
// precision, and print only needs normalized coverage. All four targets total
// 44 bytes/pixel, fitting 3440x1440 within 256 MiB per in-flight mask slot.
inline constexpr UINT64 material_mask_byte_limit = 256ull * 1024 * 1024;
inline constexpr UINT64 material_mask_bytes(UINT width, UINT height)
{
    return UINT64{width} * height * 44;
}
} // namespace dingosdk::overlay::detail
