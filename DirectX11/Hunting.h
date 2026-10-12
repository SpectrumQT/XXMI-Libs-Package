// Hunting
//  This code is to implement the Hunting mechanism as a separate compilation from the main wrapper code.
//  It implements all the shader management based on user input via key presses from Input.

#include "HackerDevice.h"

// Custom #include handler used to track which shaders need to be reloaded after an included file is modified
class MigotoIncludeHandler : public ID3DInclude
{
	std::vector<std::string> dir_stack;

	void push_dir(const char *path);
public:
	MigotoIncludeHandler(const char *path);

	STDMETHOD(Open)(D3D_INCLUDE_TYPE IncludeType, LPCSTR pFileName, LPCVOID pParentData, LPCVOID *ppData, UINT *pBytes);
	STDMETHOD(Close)(LPCVOID pData);
};

void TimeoutHuntingBuffers();
void ParseHuntingSection();
void DumpUsage(wchar_t *dir);

void RegisterVisitedIndexBufferNoLock(uint32_t hash);
void RegisterVisitedIndexBuffer(uint32_t hash);
void RegisterVisitedVertexBufferNoLock(uint32_t hash, uint32_t slot_id);
void RegisterVisitedVertexBuffer(uint32_t hash, uint32_t slot_id);
void PurgeStaleVisitedBufferHashes(HackerDevice* device);
// Once a frame while hunting, before the keys are looked at
void UpdateHuntingBufferSelection(HackerDevice* device);

// For a shader that never was a D3D11 shader object of the game: writes what
// marking_actions asks for of DXBC bytecode to ShaderFixes, without loading
// the result in place of anything
void CopyBytecodeToFixes(UINT64 hash, const char *type, const void *bytecode, size_t size);
