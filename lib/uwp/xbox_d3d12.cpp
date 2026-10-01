#include "xbox_d3d12.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <winrt/base.h>

#include "../internal.hpp"

#include <cstring>
#include <mutex>

namespace aurora::uwp {
namespace {
constexpr Module Log{"aurora::uwp::d3d12"};

// ID3D12PipelineState vtable layout: IUnknown (3), ID3D12Object (4), ID3D12DeviceChild (1), then GetCachedBlob.
constexpr size_t GetCachedBlobSlot = 8;

HRESULT STDMETHODCALLTYPE get_cached_blob_unsupported(ID3D12PipelineState*, ID3DBlob** blob) {
  if (blob != nullptr) {
    *blob = nullptr;
  }
  return E_NOTIMPL;
}

bool patch_get_cached_blob(ID3D12PipelineState* pipeline) {
  auto** vtable = *reinterpret_cast<void***>(pipeline);
  void** slot = &vtable[GetCachedBlobSlot];
  if (*slot == reinterpret_cast<void*>(&get_cached_blob_unsupported)) {
    return true;
  }
  ULONG oldProtection = 0;
  if (!VirtualProtectFromApp(slot, sizeof(void*), PAGE_READWRITE, &oldProtection)) {
    Log.error("Failed to unprotect the D3D12 pipeline state vtable: {}", GetLastError());
    return false;
  }
  *slot = reinterpret_cast<void*>(&get_cached_blob_unsupported);
  VirtualProtectFromApp(slot, sizeof(void*), oldProtection, &oldProtection);
  return true;
}

winrt::com_ptr<ID3DBlob> compile(pD3DCompile compileFn, const char* source, const char* target) {
  winrt::com_ptr<ID3DBlob> code;
  winrt::com_ptr<ID3DBlob> errors;
  const HRESULT hr =
      compileFn(source, std::strlen(source), nullptr, nullptr, nullptr, "main", target, 0, 0, code.put(), errors.put());
  if (FAILED(hr)) {
    Log.error("Failed to compile {} shader: {:#010x}", target, static_cast<uint32_t>(hr));
    return nullptr;
  }
  return code;
}

bool install() {
  // Both come with the Xbox OS.
  HMODULE d3d12 = LoadLibraryExW(L"d3d12.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  HMODULE compiler = LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (d3d12 == nullptr || compiler == nullptr) {
    Log.error("Failed to load d3d12.dll or d3dcompiler_47.dll");
    return false;
  }
  const auto createDevice =
      reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(GetProcAddress(d3d12, "D3D12CreateDevice"));
  const auto serializeRootSignature =
      reinterpret_cast<PFN_D3D12_SERIALIZE_ROOT_SIGNATURE>(GetProcAddress(d3d12, "D3D12SerializeRootSignature"));
  const auto compileFn = reinterpret_cast<pD3DCompile>(GetProcAddress(compiler, "D3DCompile"));
  if (createDevice == nullptr || serializeRootSignature == nullptr || compileFn == nullptr) {
    Log.error("Missing D3D12 or D3DCompiler entry points");
    return false;
  }

  // Pipeline state objects all share the runtime's vtables, so patching one graphics and one compute pipeline
  // covers every pipeline Dawn creates later.
  winrt::com_ptr<ID3D12Device> device;
  winrt::com_ptr<ID3DBlob> rootSignatureBlob;
  winrt::com_ptr<ID3D12RootSignature> rootSignature;
  D3D12_ROOT_SIGNATURE_DESC rootSignatureDesc{};
  if (FAILED(createDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device.put()))) ||
      FAILED(serializeRootSignature(&rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, rootSignatureBlob.put(),
                                    nullptr)) ||
      FAILED(device->CreateRootSignature(0, rootSignatureBlob->GetBufferPointer(), rootSignatureBlob->GetBufferSize(),
                                         IID_PPV_ARGS(rootSignature.put())))) {
    Log.error("Failed to create a D3D12 device for the pipeline workaround");
    return false;
  }
  const auto vs = compile(compileFn, "float4 main(uint id : SV_VertexID) : SV_Position { return 0; }", "vs_5_0");
  const auto ps = compile(compileFn, "float4 main() : SV_Target { return 0; }", "ps_5_0");
  const auto cs = compile(compileFn, "[numthreads(1, 1, 1)] void main() {}", "cs_5_0");
  if (!vs || !ps || !cs) {
    return false;
  }

  D3D12_GRAPHICS_PIPELINE_STATE_DESC graphicsDesc{};
  graphicsDesc.pRootSignature = rootSignature.get();
  graphicsDesc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
  graphicsDesc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
  graphicsDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  graphicsDesc.SampleMask = UINT_MAX;
  graphicsDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  graphicsDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  graphicsDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  graphicsDesc.NumRenderTargets = 1;
  graphicsDesc.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
  graphicsDesc.SampleDesc.Count = 1;
  D3D12_COMPUTE_PIPELINE_STATE_DESC computeDesc{};
  computeDesc.pRootSignature = rootSignature.get();
  computeDesc.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};

  winrt::com_ptr<ID3D12PipelineState> graphics;
  winrt::com_ptr<ID3D12PipelineState> compute;
  if (FAILED(device->CreateGraphicsPipelineState(&graphicsDesc, IID_PPV_ARGS(graphics.put()))) ||
      FAILED(device->CreateComputePipelineState(&computeDesc, IID_PPV_ARGS(compute.put())))) {
    Log.error("Failed to create D3D12 pipelines for the pipeline workaround");
    return false;
  }
  return patch_get_cached_blob(graphics.get()) && patch_get_cached_blob(compute.get());
}
} // namespace

bool install_xbox_d3d12_workarounds() noexcept {
  static std::once_flag once;
  static bool installed = false;
  std::call_once(once, [] {
    try {
      installed = install();
    } catch (const winrt::hresult_error& e) {
      Log.error("D3D12 workaround failed: {}", winrt::to_string(e.message()));
    }
    if (installed) {
      Log.info("Disabled ID3D12PipelineState::GetCachedBlob, which removes the device on Xbox");
    }
  });
  return installed;
}

} // namespace aurora::uwp
