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
    for (UINT i = 0; i < 3; ++i)
        result.RTFormats[i] = DXGI_FORMAT_R32G32B32A32_FLOAT;
    result.RTFormats[3] = DXGI_FORMAT_R32G32B32A32_UINT;
    return result;
}
} // namespace dingosdk::overlay::detail
