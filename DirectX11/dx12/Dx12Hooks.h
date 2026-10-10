#pragma once

#include <d3d11.h>
#include <d3d11on12.h>

// Hooks the d3d12.dll exports we need if the DLL is in the process and they
// are not hooked yet. Called from DllMain and from the LoadLibraryExW hook, so
// it has to stay within what is permitted under the loader lock.
void Dx12HookExports();

// D3D11On12CreateDevice for a caller that may be holding our proxies of the
// D3D12 device and queues: the D3D11 runtime needs the originals.
HRESULT Dx12D3D11On12CreateDevice(PFN_D3D11ON12_CREATE_DEVICE orig, IUnknown *pDevice, UINT Flags,
		const D3D_FEATURE_LEVEL *pFeatureLevels, UINT FeatureLevels, IUnknown *const *ppCommandQueues,
		UINT NumQueues, UINT NodeMask, ID3D11Device **ppDevice, ID3D11DeviceContext **ppImmediateContext,
		D3D_FEATURE_LEVEL *pChosenFeatureLevel);
