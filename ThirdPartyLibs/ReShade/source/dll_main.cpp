#include "dll_main.h"

HMODULE g_module_handle = nullptr;
std::filesystem::path g_reshade_dll_path;

/// <summary>
/// Returns the path to the module file identified by the specified <paramref name="module"/> handle.
/// </summary>
std::filesystem::path get_module_path(HMODULE module)
{
	WCHAR buf[4096];
	return std::filesystem::path(buf, buf + GetModuleFileNameW(module, buf, ARRAYSIZE(buf)));
}