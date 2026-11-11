#include <D3Dcompiler.h>

#include "log.h"
#include "DHash64Gpu.h"

using Microsoft::WRL::ComPtr;

namespace
{
    constexpr char kDHashShader[] = R"(
        Texture2D<float4> Source : register(t0);
        SamplerState LinearClamp : register(s0);
        RWStructuredBuffer<uint> Bits : register(u0);

        // 9 samples per row, 8 rows.
        groupshared float SampleValue[72];

        // One bit per comparison.
        groupshared uint HashBits[64];

        // Equal contribution from all four channels.
        static const float4 ChannelWeights = float4(0.25f, 0.25f, 0.25f, 0.25f);

        // Encode a value from 0..16 into a 16-bit thermometer code.
        uint Thermometer16(uint value)
        {
            return (1u << value) - 1u;
        }

        [numthreads(64, 1, 1)]
        void main(uint3 GroupThreadID : SV_GroupThreadID)
        {
            uint i = GroupThreadID.x;

            uint x = i & 7u;
            uint y = i >> 3u;

            // Sample the left position for each comparison.
            float2 uv = float2(((float)x + 0.5f) / 9.0f, ((float)y + 0.5f) / 8.0f);

            float4 color = Source.SampleLevel(LinearClamp, uv, 0);
            SampleValue[i + y] = dot(color, ChannelWeights);

            // Sample the rightmost column. The other right-hand samples
            // are already covered by the next thread's left-hand sample.
            if (i < 8u)
            {
                float2 uv_last = float2(8.5f / 9.0f, ((float)i + 0.5f) / 8.0f);

                float4 last = Source.SampleLevel(LinearClamp, uv_last, 0);
                SampleValue[i * 9u + 8u] = dot(last, ChannelWeights);
            }

            // All 72 samples must be available before comparison.
            GroupMemoryBarrierWithGroupSync();

            // Calculate one dHash bit per thread.
            uint sample_index = i + y;

            HashBits[i] = SampleValue[sample_index] > SampleValue[sample_index + 1u] ? 1u : 0u;

            // All 64 comparison bits must be available before packing.
            GroupMemoryBarrierWithGroupSync();

            // Pack the hash into two 32-bit words.
            if (i == 0u)
            {
                uint hash_low = 0u;
                uint hash_high = 0u;

                [unroll]
                for (uint b = 0u; b < 32u; ++b)
                {
                    hash_low |= HashBits[b] << b;
                    hash_high |= HashBits[b + 32u] << b;
                }

                // Zero dHash fallback: encode RGBA using thermometer codes.
                if ((hash_low | hash_high) == 0u)
                {
                    float4 color = Source.SampleLevel(LinearClamp, float2(0.5f, 0.5f), 0);

                    // Quantize each channel to 17 levels (0..16).
                    // Explicit floor(+0.5) matches Python's half-up rounding.
                    uint4 q = (uint4)floor(saturate(color) * 16.0f + 0.5f);

                    // Generate 16-bit thermometer codes.
                    uint r = Thermometer16(q.r);
                    uint g = Thermometer16(q.g);
                    uint b = Thermometer16(q.b);
                    uint a = Thermometer16(q.a);

                    // Pack four 16-bit channels into 64 bits.
                    // Bits[0] = lower 32 bits (B, A)
                    // Bits[1] = upper 32 bits (R, G)
                    hash_low = (b << 16u) | a;
                    hash_high = (r << 16u) | g;
                }

                // Output layout:
                // Bits[0] = hash bits  0..31
                // Bits[1] = hash bits 32..63
                Bits[0] = hash_low;
                Bits[1] = hash_high;
            }
        }
    )";
}

HRESULT DHash64Gpu::Initialize(ID3D11Device* device)
{
    if (!device)
        return E_INVALIDARG;

    if (IsInitialized())
        return S_FALSE;

    Reset();
    mOrigDevice1 = device;

    ComPtr<ID3DBlob> bytecode;
    ComPtr<ID3DBlob> errors;

    HRESULT hr = D3DCompile(
        kDHashShader,
        sizeof(kDHashShader) - 1,
        "DHash64CS.hlsl",
        nullptr,
        nullptr,
        "main",
        "cs_5_0",
        D3DCOMPILE_ENABLE_STRICTNESS,
        0,
        bytecode.GetAddressOf(),
        errors.GetAddressOf()
    );

    if (FAILED(hr))
    {
        if (errors)
            OutputDebugStringA(static_cast<const char*>(errors->GetBufferPointer()));

        Reset();
        return hr;
    }

    hr = mOrigDevice1->CreateComputeShader(
        bytecode->GetBufferPointer(),
        bytecode->GetBufferSize(),
        nullptr,
        mComputeShader.GetAddressOf()
    );

    if (FAILED(hr))
    {
        Reset();
        return hr;
    }

    // Two uint32 values = one uint64 hash.
    constexpr UINT kHashWordCount = 2;

    D3D11_SAMPLER_DESC samplerDesc{};

    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;

    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;

    samplerDesc.ComparisonFunc = D3D11_COMPARISON_ALWAYS;

    samplerDesc.MinLOD = 0.0f;
    samplerDesc.MaxLOD = 0.0f;

    hr = mOrigDevice1->CreateSamplerState(&samplerDesc, mSampler.GetAddressOf());

    if (FAILED(hr))
    {
        Reset();
        return hr;
    }

    // Two uint32 values = one uint64 hash.
    D3D11_BUFFER_DESC bufferDesc{};

    bufferDesc.ByteWidth = kHashWordCount * sizeof(uint32_t);
    bufferDesc.Usage = D3D11_USAGE_DEFAULT;
    bufferDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    bufferDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bufferDesc.StructureByteStride = sizeof(uint32_t);

    hr = mOrigDevice1->CreateBuffer(&bufferDesc, nullptr, mOutputBuffer.GetAddressOf());

    if (FAILED(hr))
    {
        Reset();
        return hr;
    }

    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};

    uavDesc.Format = DXGI_FORMAT_UNKNOWN;
    uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.FirstElement = 0;
    uavDesc.Buffer.NumElements = kHashWordCount;

    hr = mOrigDevice1->CreateUnorderedAccessView(mOutputBuffer.Get(), &uavDesc, mOutputUAV.GetAddressOf());

    if (FAILED(hr))
    {
        Reset();
        return hr;
    }

    // CPU-readable staging buffer.
    D3D11_BUFFER_DESC stagingDesc = bufferDesc;

    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    hr = mOrigDevice1->CreateBuffer(&stagingDesc, nullptr, mStagingBuffer.GetAddressOf());

    if (FAILED(hr))
    {
        Reset();
        return hr;
    }

    if (!mOrigDevice1 ||
        !mComputeShader ||
        !mSampler ||
        !mOutputBuffer ||
        !mOutputUAV ||
        !mStagingBuffer)
    {
        Reset();
        return E_FAIL;
    }

    initialized = true;
    return S_OK;
}

namespace
{
    class ScopedCSState
    {
    public:
        explicit ScopedCSState(ID3D11DeviceContext* context)
            : mOrigContext1(context)
        {
            mNumInterfaces = D3D11_SHADER_MAX_INTERFACES;

            mOrigContext1->CSGetShader(&mComputeShader, mInterfaces, &mNumInterfaces);

            mOrigContext1->CSGetShaderResources(0, 1, &mOrigSRV);
            mOrigContext1->CSGetSamplers(0, 1, &mOrigSampler);
            mOrigContext1->CSGetUnorderedAccessViews(0, 1, &mOrigUAV);
        }

        ~ScopedCSState()
        {
            mOrigContext1->CSSetShader(mComputeShader, mInterfaces, mNumInterfaces);
            mOrigContext1->CSSetShaderResources(0, 1, &mOrigSRV);
            mOrigContext1->CSSetSamplers(0, 1, &mOrigSampler);
            mOrigContext1->CSSetUnorderedAccessViews(0, 1, &mOrigUAV, nullptr);

            SafeRelease(mComputeShader);
            SafeRelease(mOrigSRV);
            SafeRelease(mOrigSampler);
            SafeRelease(mOrigUAV);

            for (auto*& instance : mInterfaces)
                SafeRelease(instance);
        }

        ScopedCSState(const ScopedCSState&) = delete;
        ScopedCSState& operator=(const ScopedCSState&) = delete;

    private:
        template<typename T>
        static void SafeRelease(T*& ptr)
        {
            if (ptr)
            {
                ptr->Release();
                ptr = nullptr;
            }
        }

        ID3D11DeviceContext* mOrigContext1;

        ID3D11ComputeShader* mComputeShader = nullptr;

        ID3D11ClassInstance* mInterfaces[D3D11_SHADER_MAX_INTERFACES]{};

        UINT mNumInterfaces = 0;

        ID3D11ShaderResourceView* mOrigSRV = nullptr;
        ID3D11SamplerState* mOrigSampler = nullptr;
        ID3D11UnorderedAccessView* mOrigUAV = nullptr;
    };
}

HRESULT DHash64Gpu::ComputeHash(
    ID3D11DeviceContext* context, ID3D11Texture2D* texture, UINT mipLevel, DXGI_FORMAT format, uint64_t& outHash
)
{
    outHash = 0;

    if (!IsInitialized())
    {
        LogDebug("DHash64Gpu::ComputeHash: service is not initialized");
        return E_UNEXPECTED;
    }

    if (!context)
    {
        LogDebug("DHash64Gpu::ComputeHash: context is null");
        return E_INVALIDARG;
    }

    if (!texture)
    {
        LogDebug("DHash64Gpu::ComputeHash: texture is null");
        return E_INVALIDARG;
    }

    if (format == DXGI_FORMAT_UNKNOWN)
    {
        LogDebug("DHash64Gpu::ComputeHash: format is DXGI_FORMAT_UNKNOWN");
        return E_INVALIDARG;
    }

    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);

    if (desc.ArraySize != 1 ||
        desc.SampleDesc.Count != 1 ||
        mipLevel >= desc.MipLevels ||
        !(desc.BindFlags & D3D11_BIND_SHADER_RESOURCE))
    {
        LogDebug(
            "DHash64Gpu::ComputeHash: unsupported texture descriptor "
            "(ArraySize=%u, SampleCount=%u, MipLevel=%u, MipLevels=%u, BindFlags=0x%08X)",
            desc.ArraySize, desc.SampleDesc.Count, mipLevel, desc.MipLevels, desc.BindFlags);

        return E_INVALIDARG;
    }

    // Create an SRV exposing only our selected mip as mip 0.
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};

    srvDesc.Format = format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MostDetailedMip = mipLevel;
    srvDesc.Texture2D.MipLevels = 1;

    ComPtr<ID3D11ShaderResourceView> mipSrv;

    UINT support = 0;

    HRESULT hr = mOrigDevice1->CheckFormatSupport(format, &support);

    if (FAILED(hr))
    {
        LogDebug("DHash64Gpu::ComputeHash: CheckFormatSupport failed: 0x%08X\n", (UINT)hr);
        return hr;
    }

    if (!(support & D3D11_FORMAT_SUPPORT_SHADER_SAMPLE))
    {
        LogDebug("DHash64Gpu::ComputeHash: Format does not support shader sampling: %u\n", (UINT)format);
    }

    hr = mOrigDevice1->CreateShaderResourceView(texture, &srvDesc, mipSrv.GetAddressOf());

    if (FAILED(hr))
    {
        LogDebug("DHash64Gpu::ComputeHash: FAILED CreateShaderResourceView\n");
        return hr;
    }

    {
        ScopedCSState savedState(context);

        ID3D11ShaderResourceView* inputSrv = mipSrv.Get();
        ID3D11SamplerState* sampler = mSampler.Get();
        ID3D11UnorderedAccessView* uav = mOutputUAV.Get();

        context->CSSetShader(mComputeShader.Get(), nullptr, 0);

        context->CSSetShaderResources(0, 1, &inputSrv);
        context->CSSetSamplers(0, 1, &sampler);

        context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);

        context->Dispatch(1, 1, 1);

        // Unbind output before CopyResource.
        ID3D11UnorderedAccessView* nullUav = nullptr;

        context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);

        ID3D11ShaderResourceView* nullSrv = nullptr;

        context->CSSetShaderResources(0, 1, &nullSrv);
    }

    // Readback after restoring the application's CS state.
    context->CopyResource(mStagingBuffer.Get(), mOutputBuffer.Get());

    D3D11_MAPPED_SUBRESOURCE mapped{};

    hr = context->Map(mStagingBuffer.Get(), 0, D3D11_MAP_READ, 0, &mapped);

    if (FAILED(hr))
    {
        LogDebug("DHash64Gpu::ComputeHash: FAILED Map\n");
        return hr;
    }

    const auto* words = static_cast<const uint32_t*>(mapped.pData);

    outHash = static_cast<uint64_t>(words[0]) | (static_cast<uint64_t>(words[1]) << 32);

    context->Unmap(mStagingBuffer.Get(), 0);

    return S_OK;
}

void DHash64Gpu::Reset() noexcept
{
    mStagingBuffer.Reset();
    mOutputUAV.Reset();
    mOutputBuffer.Reset();

    mSampler.Reset();
    mComputeShader.Reset();

    mOrigDevice1.Reset();

    initialized = false;
}
