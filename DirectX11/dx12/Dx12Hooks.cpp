#include "Dx12Hooks.h"

#include <vector>

#include "Bridge.h"
#include "Dx12Common.h"
#include "Dx12CommandQueue.h"
#include "Dx12Device.h"

#include "util.h"
#include "../cursor.h" // For InstallHookLate
#include "../D3D11Wrapper.h"
#include "../DLLMainHook.h"
#include "../Globals.h"
#include "../HookedDXGI.h"

static PFN_D3D12_CREATE_DEVICE fnOrigD3D12CreateDevice;
static PFN_D3D12_GET_INTERFACE fnOrigD3D12GetInterface;

static bool dx12_enabled()
{
	InitCommon();

	return G->bIntendedTargetExe && G->dx12 != Dx12Mode::OFF;
}

// The debug layer has to be enabled before the device is created. What it has
// to say is collected from the device's info queue, see Dx12LogDebugMessages.
static void enable_debug_layer()
{
	static bool enabled;
	HRESULT (WINAPI *get_debug_interface)(REFIID, void**);
	ID3D12Debug *debug = NULL;

	if (enabled || !dx12_enabled() || !G->dx12_debug_layer)
		return;
	enabled = true;

	*(FARPROC*)&get_debug_interface = GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "D3D12GetDebugInterface");
	if (!get_debug_interface || FAILED(get_debug_interface(IID_PPV_ARGS(&debug)))) {
		Dx12LogInfo("*** dx12_debug_layer: the debug layer is not available, it comes with the Graphics Tools optional feature of Windows\n");
		return;
	}

	debug->EnableDebugLayer();
	debug->Release();
	Dx12LogInfo("dx12_debug_layer: debug layer enabled\n");
}

static void log_device_created(const char *func, IUnknown *adapter, D3D_FEATURE_LEVEL feature_level,
		REFIID riid, void **ppDevice, HRESULT hr)
{
	// Engines test for D3D12 support by asking for a device without
	// providing anywhere to return it.
	if (!ppDevice) {
		Dx12LogDebug("%s support check (adapter=%p, feature level=0x%x, riid=%s) = 0x%x\n",
			func, adapter, feature_level, NameFromIID(riid).c_str(), hr);
		return;
	}

	Dx12LogInfo("%s(adapter=%p, feature level=0x%x, riid=%s) = 0x%x, device=%p\n",
		func, adapter, feature_level, NameFromIID(riid).c_str(), hr, SUCCEEDED(hr) ? *ppDevice : NULL);
}

static void device_created(const char *func, IUnknown *adapter, D3D_FEATURE_LEVEL feature_level,
		REFIID riid, void **ppDevice, HRESULT hr)
{
	if (!dx12_enabled())
		return;

	log_device_created(func, adapter, feature_level, riid, ppDevice, hr);

	if (FAILED(hr) || !ppDevice || !*ppDevice)
		return;

	// Set while we are inside a call into DXGI or D3D11, where a device
	// is being created for the runtime's own use (D3D11On12) or for a
	// driver layer, neither of which expects our proxy.
	if (get_tls()->hooking_quirk_protection) {
		Dx12LogInfo("  Created from inside another hooked call, not wrapping\n");
		return;
	}

	if (Dx12IsD3D11Game()) {
		Dx12LogInfo("  The game presents with D3D11, not wrapping\n");
		return;
	}

	Dx12WrapDevice(riid, ppDevice);
}

// -----------------------------------------------------------------------------
// With the Agility SDK a game can create its device through
// ID3D12DeviceFactory instead of D3D12CreateDevice. The factory comes from
// D3D12GetInterface, either directly or via ID3D12SDKConfiguration1, and
// neither interface has an export we could hook, so their vtables are patched
// when we first see them.
//
// Every D3D12Core.dll in the process (the system one and any the game ships)
// has its own factory class, so the original method is remembered per vtable.

struct VtblPatch {
	void **vtbl;
	void *orig;
};

struct VtblPatchTable {
	const char *name;
	unsigned slot;
	void *hook;
	VtblPatch patches[4];
	unsigned count;
};

static SRWLOCK vtbl_patch_lock = SRWLOCK_INIT;

static void patch_vtable(VtblPatchTable *table, IUnknown *object)
{
	void **vtbl = *(void***)object;
	VtblPatch *patch;
	unsigned i;

	AcquireSRWLockExclusive(&vtbl_patch_lock);

	for (i = 0; i < table->count; i++) {
		if (table->patches[i].vtbl == vtbl)
			goto out_unlock;
	}

	if (table->count == ARRAYSIZE(table->patches)) {
		Dx12LogInfo("*** Too many different vtables for %s, not hooking %p\n", table->name, vtbl);
		goto out_unlock;
	}

	patch = &table->patches[table->count];
	patch->orig = NULL;
	hook_vtable_slot(table->name, &vtbl[table->slot], table->hook, &patch->orig);
	if (patch->orig) {
		patch->vtbl = vtbl;
		table->count++;
	}

out_unlock:
	ReleaseSRWLockExclusive(&vtbl_patch_lock);
}

static void* orig_method(VtblPatchTable *table, void *object)
{
	void **vtbl = *(void***)object;
	void *orig = NULL;
	unsigned i;

	AcquireSRWLockShared(&vtbl_patch_lock);
	for (i = 0; i < table->count; i++) {
		if (table->patches[i].vtbl == vtbl) {
			orig = table->patches[i].orig;
			break;
		}
	}
	ReleaseSRWLockShared(&vtbl_patch_lock);

	return orig;
}

static HRESULT STDMETHODCALLTYPE Hooked_DeviceFactory_CreateDevice(ID3D12DeviceFactory *This,
		IUnknown *adapter, D3D_FEATURE_LEVEL FeatureLevel, REFIID riid, void **ppvDevice);
static HRESULT STDMETHODCALLTYPE Hooked_SDKConfiguration1_CreateDeviceFactory(ID3D12SDKConfiguration1 *This,
		UINT SDKVersion, LPCSTR SDKPath, REFIID riid, void **ppvFactory);

// Slots are counted from IUnknown::QueryInterface at 0. The device factory
// in the 10.0.22621 SDK headers is a preview that lacks the feature level
// argument of CreateDevice, these hooks follow the released interface.
static VtblPatchTable device_factory_create_device = {
	"ID3D12DeviceFactory::CreateDevice", 9, Hooked_DeviceFactory_CreateDevice
};
static VtblPatchTable sdk_configuration_create_device_factory = {
	"ID3D12SDKConfiguration1::CreateDeviceFactory", 4, Hooked_SDKConfiguration1_CreateDeviceFactory
};

static void hook_device_factory(IUnknown *unknown)
{
	ID3D12DeviceFactory *factory = NULL;

	if (FAILED(unknown->QueryInterface(IID_PPV_ARGS(&factory))))
		return;

	patch_vtable(&device_factory_create_device, factory);
	factory->Release();
}

static void hook_sdk_configuration(IUnknown *unknown)
{
	ID3D12SDKConfiguration1 *config = NULL;

	if (FAILED(unknown->QueryInterface(IID_PPV_ARGS(&config))))
		return;

	patch_vtable(&sdk_configuration_create_device_factory, config);
	config->Release();
}

static HRESULT STDMETHODCALLTYPE Hooked_DeviceFactory_CreateDevice(ID3D12DeviceFactory *This,
		IUnknown *adapter, D3D_FEATURE_LEVEL FeatureLevel, REFIID riid, void **ppvDevice)
{
	typedef HRESULT (STDMETHODCALLTYPE *CreateDevice_t)(ID3D12DeviceFactory*, IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
	CreateDevice_t orig = (CreateDevice_t)orig_method(&device_factory_create_device, This);
	HRESULT hr;

	if (!orig) {
		Dx12LogInfo("*** ID3D12DeviceFactory::CreateDevice called on unknown vtable of %p\n", This);
		return E_FAIL;
	}

	enable_debug_layer();
	hr = orig(This, adapter, FeatureLevel, riid, ppvDevice);
	device_created("ID3D12DeviceFactory::CreateDevice", adapter, FeatureLevel, riid, ppvDevice, hr);

	return hr;
}

static HRESULT STDMETHODCALLTYPE Hooked_SDKConfiguration1_CreateDeviceFactory(ID3D12SDKConfiguration1 *This,
		UINT SDKVersion, LPCSTR SDKPath, REFIID riid, void **ppvFactory)
{
	typedef HRESULT (STDMETHODCALLTYPE *CreateDeviceFactory_t)(ID3D12SDKConfiguration1*, UINT, LPCSTR, REFIID, void**);
	CreateDeviceFactory_t orig = (CreateDeviceFactory_t)orig_method(&sdk_configuration_create_device_factory, This);
	HRESULT hr;

	if (!orig) {
		Dx12LogInfo("*** ID3D12SDKConfiguration1::CreateDeviceFactory called on unknown vtable of %p\n", This);
		return E_FAIL;
	}

	hr = orig(This, SDKVersion, SDKPath, riid, ppvFactory);
	if (FAILED(hr) || !ppvFactory || !*ppvFactory || !dx12_enabled())
		return hr;

	Dx12LogInfo("ID3D12SDKConfiguration1::CreateDeviceFactory(SDKVersion=%u, SDKPath=%s, riid=%s) = %p\n",
		SDKVersion, SDKPath ? SDKPath : "", NameFromIID(riid).c_str(), *ppvFactory);
	hook_device_factory((IUnknown*)*ppvFactory);

	return hr;
}

// -----------------------------------------------------------------------------

static HRESULT WINAPI Hooked_D3D12CreateDevice(IUnknown *pAdapter, D3D_FEATURE_LEVEL MinimumFeatureLevel,
		REFIID riid, void **ppDevice)
{
	HRESULT hr;

	enable_debug_layer();
	hr = fnOrigD3D12CreateDevice(pAdapter, MinimumFeatureLevel, riid, ppDevice);

	device_created("D3D12CreateDevice", pAdapter, MinimumFeatureLevel, riid, ppDevice, hr);

	return hr;
}

static HRESULT WINAPI Hooked_D3D12GetInterface(REFCLSID rclsid, REFIID riid, void **ppv)
{
	HRESULT hr = fnOrigD3D12GetInterface(rclsid, riid, ppv);

	if (FAILED(hr) || !ppv || !*ppv || !dx12_enabled())
		return hr;

	Dx12LogInfo("D3D12GetInterface(rclsid=%s, riid=%s) = %p\n",
		NameFromIID(rclsid).c_str(), NameFromIID(riid).c_str(), *ppv);

	if (rclsid == CLSID_D3D12DeviceFactory)
		hook_device_factory((IUnknown*)*ppv);
	else if (rclsid == CLSID_D3D12SDKConfiguration)
		hook_sdk_configuration((IUnknown*)*ppv);

	return hr;
}

HRESULT Dx12D3D11On12CreateDevice(PFN_D3D11ON12_CREATE_DEVICE orig, IUnknown *pDevice, UINT Flags,
		const D3D_FEATURE_LEVEL *pFeatureLevels, UINT FeatureLevels, IUnknown *const *ppCommandQueues,
		UINT NumQueues, UINT NodeMask, ID3D11Device **ppDevice, ID3D11DeviceContext **ppImmediateContext,
		D3D_FEATURE_LEVEL *pChosenFeatureLevel)
{
	Dx12Device *device = Dx12QueryProxy<Dx12Device>(pDevice);
	std::vector<Dx12CommandQueue*> proxies;
	std::vector<IUnknown*> queues;
	HRESULT hr;
	UINT i;

	if (device)
		pDevice = device->mOrig;

	if (ppCommandQueues) {
		for (i = 0; i < NumQueues; i++) {
			Dx12CommandQueue *proxy = Dx12QueryProxy<Dx12CommandQueue>(ppCommandQueues[i]);

			queues.push_back(proxy ? proxy->mOrig : ppCommandQueues[i]);
			if (proxy)
				proxies.push_back(proxy);
		}
		ppCommandQueues = queues.data();
	}

	hr = orig(pDevice, Flags, pFeatureLevels, FeatureLevels, ppCommandQueues, NumQueues, NodeMask,
		ppDevice, ppImmediateContext, pChosenFeatureLevel);

	for (Dx12CommandQueue *proxy : proxies)
		proxy->Release();
	if (device)
		device->Release();

	return hr;
}

void Dx12HookExports()
{
	static volatile LONG hooked = 0;
	HINSTANCE d3d12;

	if (hooked)
		return;

	d3d12 = NktHookLibHelpers::GetModuleBaseAddress(L"d3d12.dll");
	if (!d3d12)
		return;

	if (InterlockedCompareExchange(&hooked, 1, 0) != 0)
		return;

	InstallHookLate(d3d12, "D3D12CreateDevice", (void**)&fnOrigD3D12CreateDevice, Hooked_D3D12CreateDevice);
	// Not exported by the d3d12.dll of older Windows 10 builds, which do
	// not have the device factory either.
	InstallHookLate(d3d12, "D3D12GetInterface", (void**)&fnOrigD3D12GetInterface, Hooked_D3D12GetInterface);
}
