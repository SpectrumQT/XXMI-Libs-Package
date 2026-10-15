#pragma once

#include <d3d11_1.h>
#include <cstdint>
#include <string>

// A whole file read into memory. One ReadFile into a buffer kept for the
// next file is much faster than faulting the pages of a mapped view in one
// by one while a D3D create call copies from it, and the buffer is warm in
// the cache for that copy.
class FileContents
{
public:
	explicit FileContents(const wchar_t *path);
	~FileContents();

	FileContents(const FileContents&) = delete;
	FileContents& operator=(const FileContents&) = delete;

	bool valid() const { return data != nullptr; }

	const uint8_t *data = nullptr;
	size_t size = 0;
	DWORD error = 0;

private:
	uint8_t *buffer = nullptr;
	size_t capacity = 0;
};

enum class SharedResourceLoader {
	DDS,
	WIC,
	BUFFER,
};

// Identifies a GPU resource created from a file by the file's contents and
// everything that affected its creation, so that two [Resource] sections
// pointing at identical files end up with the same ID3D11Resource:
struct SharedResourceKey
{
	ID3D11Device *device = nullptr;
	SharedResourceLoader loader = SharedResourceLoader::DDS;
	uint32_t bind_flags = 0;
	uint32_t misc_flags = 0;
	// DDS forceSRGB or the WIC loader flags:
	uint32_t load_flags = 0;
	// Buffers only, as created after the ini overrides:
	uint32_t byte_width = 0;
	uint32_t structure_stride = 0;

	// Filled in by FindSharedFileResource() from the data the resource is
	// created from. The crc32c only narrows down the candidates:
	uint64_t size = 0;
	uint32_t content = 0;

	bool operator==(const SharedResourceKey &other) const;
};

// The registry holds a reference of its own on every shared resource until
// ReleaseSharedFileResources() is called, so that a hit is never a dangling
// pointer. A hit means the data is byte for byte identical to the file an
// earlier resource with the same key was created from, and the returned
// resource is AddRef()ed for the caller. On a miss the key is complete and
// can be passed to ShareFileResource(), together with the path of the file,
// once the resource has been created from the same data.
ID3D11Resource* FindSharedFileResource(SharedResourceKey &key, const void *data, size_t size);
void ShareFileResource(const SharedResourceKey &key, const std::wstring &path, ID3D11Resource *resource);
// Drops the registry's references, all of them or those on one device:
void ReleaseSharedFileResources(ID3D11Device *device = nullptr);

// Appends a resource file to the list the next session reads into the OS
// file cache ahead of the file's first use:
void RecordResourceFileUse(const std::wstring &path);
void StartResourceFilePrefetch();
void StopResourceFilePrefetch();
