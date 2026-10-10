#pragma once

#include <stdint.h>

#include <d3d12.h>
#include <dxgi1_5.h>

#include "log.h"

#define Dx12LogInfo(fmt, ...) LogInfo("[DX12] " fmt, __VA_ARGS__)
#define Dx12LogDebug(fmt, ...) LogDebug("[DX12] " fmt, __VA_ARGS__)

// The D3D12 objects the game sees are proxies: COM objects of our own that
// derive from the newest version of the interface in the SDK and forward to
// the original. generated/*.inl holds the forwarding half of each class, see
// tools/gen_dx12_proxies.py.
//
// Reference counting follows the original object: every reference on a proxy
// is also a reference on its original, so the counts the game reads back stay
// what it expects, and the proxy goes away with its last reference.
//
// A proxy asks the original for the newest interface it implements once, when
// it is created, and from then on answers QueryInterface for that version and
// everything below it with itself. Anything else is forwarded, which hands out
// an unwrapped interface: fine for the side interfaces that exist today, but a
// newer ID3D12Device or ID3D12GraphicsCommandList than this SDK knows would
// bypass us, hence the log line.
void Dx12LogForwardedQuery(const char *proxy, REFIID riid, HRESULT hr);

// Returns the original of an object that may be one of our proxies. Proxy
// classes are final, so comparing the vtable pointer identifies them without
// a QueryInterface round trip, which matters for ExecuteCommandLists and
// SetDescriptorHeaps.
template <class Proxy, class Interface>
static inline Interface* Dx12Unwrap(Interface *object)
{
	if (object && *(void**)object == Proxy::sVtbl)
		return static_cast<Proxy*>(object)->mOrig;
	return object;
}

// Finds our proxy behind an arbitrary interface pointer, which may be another
// layer's wrapper around it. The caller releases the result.
template <class Proxy>
static inline Proxy* Dx12QueryProxy(IUnknown *unknown)
{
	Proxy *proxy = NULL;

	if (!unknown || FAILED(unknown->QueryInterface(Proxy::ProxyIID, (void**)&proxy)))
		return NULL;
	return proxy;
}

// For the interface version tables of the proxies, ordered oldest first.
// Returns the index of the newest interface the object implements and that
// interface, with a reference, in *newest.
static inline int Dx12QueryNewest(IUnknown *unknown, const IID *const *iids, int count, void **newest)
{
	int i;

	for (i = count - 1; i >= 0; i--) {
		if (SUCCEEDED(unknown->QueryInterface(*iids[i], newest)))
			return i;
	}
	*newest = NULL;
	return -1;
}

static inline int Dx12FindIID(const IID *const *iids, int count, REFIID riid)
{
	int i;

	for (i = 0; i < count; i++) {
		if (IsEqualIID(*iids[i], riid))
			return i;
	}
	return -1;
}

// Counters since the last report, see Dx12FramePresented.
struct Dx12Stats {
	volatile LONG64 draws;
	volatile LONG64 dispatches;
	volatile LONG64 lists_closed;
	volatile LONG64 lists_executed;
	volatile LONG64 execute_calls;
};
extern Dx12Stats dx12_stats;

// Presents so far, for telling how long ago something happened
extern volatile UINT dx12_frame;

void Dx12FramePresented();

// crc32c_hw for memory that may be an upload heap. Those are write combined,
// where ordinary reads are an order of magnitude slower than the streaming
// loads this uses.
uint32_t Dx12Crc32cWriteCombined(uint32_t hash, const BYTE *src, size_t length);
