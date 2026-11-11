#pragma once

#include <d3d11_1.h>
#include <wrl/client.h>
#include <cstdint>

class DHash64Gpu final
{
public:
    HRESULT Initialize(ID3D11Device* device);

    HRESULT ComputeHash(ID3D11DeviceContext* context, ID3D11Texture2D* texture, UINT mipLevel, DXGI_FORMAT srvFormat, uint64_t& outHash);

    bool IsInitialized() noexcept
    {
        return initialized;
    }

private:
    void Reset() noexcept;

    bool initialized = false;

    Microsoft::WRL::ComPtr<ID3D11Device> mOrigDevice1;

    Microsoft::WRL::ComPtr<ID3D11ComputeShader> mComputeShader;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> mSampler;

    Microsoft::WRL::ComPtr<ID3D11Buffer> mOutputBuffer;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> mOutputUAV;

    Microsoft::WRL::ComPtr<ID3D11Buffer> mStagingBuffer;
};
