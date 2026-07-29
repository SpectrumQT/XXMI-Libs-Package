#include <algorithm>

#include "util.h"

#include "HackerInputLayout.h"

uint64_t CalculateInputLayoutHash(const D3D11_INPUT_ELEMENT_DESC* pElements, UINT numElements, const void* shaderSignature, SIZE_T signatureSize)
{
	uint32_t layoutHash = crc32c_hw(0, &numElements, sizeof(numElements));

	for (UINT i = 0; i < numElements; ++i)
	{
		const D3D11_INPUT_ELEMENT_DESC& element = pElements[i];

		if (element.SemanticName)
		{
			// Normalize semantic name the same way HackerInputLayout does (case-insensitive comparison)
			for (const char* p = element.SemanticName; *p; ++p)
			{
				char c = static_cast<char>(std::toupper(static_cast<unsigned char>(*p)));
				layoutHash = crc32c_hw(layoutHash, &c, sizeof(c));
			}
		}

		layoutHash = crc32c_hw(layoutHash, &element.SemanticIndex, sizeof(element.SemanticIndex));
		layoutHash = crc32c_hw(layoutHash, &element.Format, sizeof(element.Format));
		layoutHash = crc32c_hw(layoutHash, &element.InputSlot, sizeof(element.InputSlot));
		layoutHash = crc32c_hw(layoutHash, &element.AlignedByteOffset, sizeof(element.AlignedByteOffset));
		layoutHash = crc32c_hw(layoutHash, &element.InputSlotClass, sizeof(element.InputSlotClass));
		layoutHash = crc32c_hw(layoutHash, &element.InstanceDataStepRate, sizeof(element.InstanceDataStepRate));
	}

	uint32_t signatureHash = 0;
	if (shaderSignature && signatureSize)
		signatureHash = crc32c_hw(0, shaderSignature, signatureSize);

	return (static_cast<uint64_t>(signatureHash) << 32) | static_cast<uint64_t>(layoutHash);
}

HackerInputLayout::HackerInputLayout(
	ID3D11InputLayout* orig, const D3D11_INPUT_ELEMENT_DESC* pElements, UINT numElements, const void* shaderSignature, SIZE_T signatureSize, uint64_t hash
) : mOrigLayout(orig)
{
	mElements.resize(numElements);
	mSemanticNames.resize(numElements);

	for (UINT i = 0; i < numElements; ++i)
	{
		mElements[i] = pElements[i];

		if (pElements[i].SemanticName)
		{
			mSemanticNames[i] = pElements[i].SemanticName;
			std::transform(mSemanticNames[i].begin(), mSemanticNames[i].end(), mSemanticNames[i].begin(), ::toupper);
			mElements[i].SemanticName = mSemanticNames[i].c_str();
		}
		else
		{
			mElements[i].SemanticName = nullptr;
		}
	}

	mShaderSignature.resize(signatureSize);
	memcpy(mShaderSignature.data(), shaderSignature, signatureSize);

	mLayoutHash = hash ? hash : CalculateInputLayoutHash(pElements, numElements, shaderSignature, signatureSize);
}

HackerInputLayout::~HackerInputLayout()
{
}

ID3D11InputLayout* HackerInputLayout::GetOrigInputLayout() const
{
	return mOrigLayout;
}

const void* HackerInputLayout::GetShaderSignature() const
{
	return mShaderSignature.empty() ? nullptr : mShaderSignature.data();
}

SIZE_T HackerInputLayout::GetShaderSignatureSize() const
{
	return static_cast<SIZE_T>(mShaderSignature.size());
}

UINT HackerInputLayout::GetElementCount() const
{
	return static_cast<UINT>(mElements.size());
}

const D3D11_INPUT_ELEMENT_DESC* HackerInputLayout::GetElements() const
{
	return mElements.data();
}

uint64_t HackerInputLayout::GetLayoutHash() const
{
	return mLayoutHash;
}

#pragma region IUnknown
HRESULT STDMETHODCALLTYPE HackerInputLayout::QueryInterface(REFIID riid, void** ppvObject)
{
	if (!ppvObject)
		return E_POINTER;

	*ppvObject = nullptr;

	if (riid == __uuidof(IUnknown))
	{
		*ppvObject = static_cast<IUnknown*>(static_cast<ID3D11InputLayout*>(this));
	}
	else if (riid == __uuidof(ID3D11DeviceChild))
	{
		*ppvObject = static_cast<ID3D11DeviceChild*>(this);
	}
	else if (riid == __uuidof(ID3D11InputLayout))
	{
		*ppvObject = static_cast<ID3D11InputLayout*>(this);
	}
	else
	{
		return E_NOINTERFACE;
	}

	AddRef();
	return S_OK;
}

ULONG STDMETHODCALLTYPE HackerInputLayout::AddRef()
{
	return ++mRefCount;
}

ULONG STDMETHODCALLTYPE HackerInputLayout::Release()
{
	ULONG ref = --mRefCount;

	if (ref == 0)
	{
		if (mOrigLayout)
			mOrigLayout->Release();
		delete this;
	}

	return ref;
}
#pragma endregion IUnknown

#pragma region ID3D11DeviceChild
void STDMETHODCALLTYPE HackerInputLayout::GetDevice(ID3D11Device** ppDevice)
{
	mOrigLayout->GetDevice(ppDevice);
}

HRESULT STDMETHODCALLTYPE HackerInputLayout::GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData)
{
	return mOrigLayout->GetPrivateData(guid, pDataSize, pData);
}

HRESULT STDMETHODCALLTYPE HackerInputLayout::SetPrivateData(REFGUID guid, UINT DataSize, const void* pData)
{
	return mOrigLayout->SetPrivateData(guid, DataSize, pData);
}

HRESULT STDMETHODCALLTYPE HackerInputLayout::SetPrivateDataInterface(REFGUID guid, const IUnknown* pData)
{
	return mOrigLayout->SetPrivateDataInterface(guid, pData);
}
#pragma endregion ID3D11DeviceChild
