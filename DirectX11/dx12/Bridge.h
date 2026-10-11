#pragma once

#include <atomic>
#include <vector>

#include <d3d11_1.h>

#include "Dx12Common.h"

// The 3DMigoto engine (command lists, resource loading, overrides) only knows
// D3D11. On D3D12 it is given a D3D11 device and context of our own making,
// the shim, which turn what it asks for into D3D12 objects and commands:
//
//   ShimDevice, ShimObjects   D3D11 objects backed by D3D12 ones
//   ShimContext               the bindings the engine has set, and the calls
//                             that act on them
//   ShimPipeline              what a draw needs made out of those bindings:
//                             root signature, pipeline state, descriptors
//   Bridge (this)             where the resulting commands go, and the
//                             lifetime of everything they refer to
//
// The commands are recorded to a command list of our own that is executed on
// the game's queue when the frame is presented (PresentPass). Anything those
// commands use must outlive their execution, which is tracked with one fence:
// every submission signals the next value, and whatever was retired while
// recording for a submission is only freed once its value has been reached.
//
// The engine and everything here only do one thing at a time. With the game
// recording command lists on several threads and presenting on another, that
// takes a lock: whoever calls into the engine or the shim holds Dx12ShimLock.
//
// Besides the frame's own work at present, the engine gets to run for draws
// of the game it has overrides for (DrawPath). What it draws then goes to the
// game's command list, which is executed whenever the game sees fit, and the
// following keeps that safe:
//
//   - Our own list is submitted before the lock is let go of, so whatever a
//     mod resource needed done to it has been executed by the time a game
//     list recorded later can use it.
//   - Every submission leaves our resources in the common state, from where
//     D3D12 lets a list read them without a barrier. The game's lists never
//     get barriers of ours: they are executed in an order we do not know.
//   - What a game list uses is pinned to it (Dx12ShimPin), and lives until
//     that list has been executed.

class Dx12CommandQueue;
class Dx12Device;
class Dx12GraphicsCommandList;
class HackerContext;
class HackerDevice;
class ShimContext;
class ShimDevice;
struct ShimResource;

struct Dx12Shim {
	ID3D12Device *device;
	// The game's queue our commands are executed on
	ID3D12CommandQueue *queue;

	ShimDevice *shim_device;
	ShimContext *shim_context;
	HackerDevice *hacker_device;
	HackerContext *hacker_context;
};

// NULL until Dx12ShimCreate succeeded
extern Dx12Shim *dx12_shim;

// Set when the DLL is being unloaded. What the engine keeps in static
// variables is released after that, when neither the D3D12 objects behind it
// nor our own state can be relied on any more, so from then on the objects
// of the shim are left to the process going away.
extern bool dx12_unloading;

// Set once a D3D11 swap chain of ours has presented a frame. Creating one is
// not enough to go by: overlays make swap chains they never present, to find
// the methods to hook.
extern std::atomic<bool> dx12_d3d11_presented;

// Whether the game is taken to be a D3D11 game, where a D3D12 device belongs
// to an overlay or a driver layer and neither that nor the D3D11 engine
// expects the D3D12 side to step in. Whichever API presents first has the
// game, unless dx12=1 says it is D3D12.
bool Dx12IsD3D11Game();

bool Dx12ShimCreate(Dx12Device *device, Dx12CommandQueue *queue);
// For when the swap chain being presented is on another queue than before
void Dx12ShimSetQueue(Dx12CommandQueue *queue);

void Dx12ShimNotImplemented(const char *method, bool *reported);

void Dx12ShimLock();
void Dx12ShimUnlock();

// Moves what the debug layer has said since the last call to our log. Does
// nothing unless the device was created with dx12_debug_layer.
void Dx12LogDebugMessages();

// The command list to record to. Stays valid until the next submission.
ID3D12GraphicsCommandList* Dx12ShimList();
// Has the queue execute what was recorded, if anything
void Dx12ShimSubmit();
// The same, and does not return before the GPU is done with it
void Dx12ShimSubmitAndWait();

// The fence value that will be signalled once the commands being recorded now
// have been executed
UINT64 Dx12ShimPendingFence();
bool Dx12ShimFenceReached(UINT64 value);
// Counts submissions. Memory from Dx12ShimUpload and descriptors from
// Dx12ShimAllocViews are gone when it changes.
extern UINT dx12_shim_epoch;

// Releases the object once nothing recorded so far can still be using it
void Dx12ShimDeferRelease(IUnknown *object);

// Memory the GPU can read, valid for the commands of this submission. *buffer
// and *offset say where it is for a copy, *va for a view.
BYTE* Dx12ShimUpload(UINT64 size, UINT64 alignment, ID3D12Resource **buffer, UINT64 *offset, UINT64 *va);

// Descriptors a shader can be given, valid for the commands of this
// submission. Both make their heap one of the two current ones, which the
// next Dx12ShimBindHeaps sets on the list.
D3D12_CPU_DESCRIPTOR_HANDLE Dx12ShimAllocViews(UINT count, D3D12_GPU_DESCRIPTOR_HANDLE *gpu);
// Makes sure the next count calls of Dx12ShimSamplerTable end up in one heap
void Dx12ShimReserveSamplers(UINT count);
// A table of count samplers, reused if the same table was asked for before
D3D12_GPU_DESCRIPTOR_HANDLE Dx12ShimSamplerTable(const D3D12_CPU_DESCRIPTOR_HANDLE *samplers, UINT count);
void Dx12ShimBindHeaps();
extern UINT dx12_shim_view_increment;

// Descriptors that stay where they are for as long as the view or sampler
// they belong to lives. Not visible to shaders: Dx12ShimAllocViews gets a copy.
D3D12_CPU_DESCRIPTOR_HANDLE Dx12ShimAllocDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE type);
void Dx12ShimFreeDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE type, D3D12_CPU_DESCRIPTOR_HANDLE handle);

// Records the barrier that takes a resource of ours to the state, if it is
// not there already. Read states accumulate, as a resource may be read in
// several ways by one draw.
void Dx12ShimTransition(ShimResource *resource, D3D12_RESOURCE_STATES state);
void Dx12ShimForgetResource(ShimResource *resource);

// Keeps the object alive until the game's list has been executed, or has been
// reset without being executed
void Dx12ShimPin(Dx12GraphicsCommandList *list, IUnknown *object);
// For the queue proxy, after it has handed lists to the queue, with the pins
// of all of them, and for a list that drops what it recorded
void Dx12ShimPinsExecuted(ID3D12CommandQueue *queue, std::vector<IUnknown*> *pins);
void Dx12ShimPinsAbandoned(std::vector<IUnknown*> *pins);

// Makes a resource of ours usable by a game list that is being recorded.
// False if it has no D3D12 resource.
bool Dx12ShimUseOnGameList(ShimResource *resource, Dx12GraphicsCommandList *list);
// As Dx12ShimBufferAddress, for a game list. size is how much can be read
// from the address, which for a buffer without a resource is its size rounded
// up to what a constant buffer view takes.
UINT64 Dx12ShimGameBufferAddress(ID3D11Buffer *buffer, Dx12GraphicsCommandList *list, UINT *size);
// The same for a view of elements of stride bytes, which needs the resource
// and has to start at a whole number of them: size bytes of the buffer's
// contents are at offset in *resource.
bool Dx12ShimGameBufferView(ID3D11Buffer *buffer, Dx12GraphicsCommandList *list, UINT stride, ID3D12Resource **resource,
		UINT64 *offset, UINT *size);

// Once per present, with the lock held
void Dx12ShimFramePresented();
