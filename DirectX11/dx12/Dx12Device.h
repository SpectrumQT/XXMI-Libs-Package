#pragma once

#include "Dx12Common.h"

class Dx12Device final : public ID3D12Device14
{
public:
	static const GUID ProxyIID;
	static void *sVtbl;

	ID3D12Device14 *mOrig;
	// Highest N of the ID3D12DeviceN the original implements
	int mVersion;
	volatile LONG mRef;

	Dx12Device(ID3D12Device *orig);

	bool Supports(REFIID riid);

#include "generated/Dx12Device.decl.inl"
};

// Replaces a device just returned by D3D12 with its proxy. A device is a
// singleton per adapter, so this is the same proxy for as long as it lives.
// Leaves the device alone if riid is not an interface the proxy implements.
void Dx12WrapDevice(REFIID riid, void **ppDevice);
