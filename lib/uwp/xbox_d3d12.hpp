#pragma once

#ifdef AURORA_UWP

namespace aurora::uwp {

// On Xbox, ID3D12PipelineState::GetCachedBlob is unsupported: calling it removes the device
// (DXGI_ERROR_DRIVER_INTERNAL_ERROR). Dawn calls it after creating every pipeline that wasn't
// in its cache, so the D3D12 backend can't run there as is. This replaces the method in the
// D3D12 runtime's pipeline vtables with one that fails with E_NOTIMPL, which Dawn tolerates
// (it just skips caching that pipeline). Must be called before Dawn creates its D3D12 device.
bool install_xbox_d3d12_workarounds() noexcept;

} // namespace aurora::uwp

#endif
