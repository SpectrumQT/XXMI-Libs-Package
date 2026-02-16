#include "Injector.h"

#include <windows.h>
#include <stdio.h>
#include <tlhelp32.h>
#include <set>

#define DLL_EXPORT extern "C" __declspec(dllexport)


HMODULE *module = new HMODULE;


static bool verify_injection(PROCESSENTRY32 *pe, const wchar_t *module, bool log_name)
{
	HANDLE snapshot;
	MODULEENTRY32 me;
	const wchar_t *basename = wcsrchr(module, '\\');
	bool rc = false;
	static std::set<DWORD> pids;
	wchar_t exe_path[MAX_PATH], mod_path[MAX_PATH];

	if (basename)
		basename++;
	else
		basename = module;

	do {
		snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pe->th32ProcessID);
	} while (snapshot == INVALID_HANDLE_VALUE && GetLastError() == ERROR_BAD_LENGTH);
	if (snapshot == INVALID_HANDLE_VALUE) {
		printf("%S (%d): Verification Failed: Invalid Handle: %d\n", pe->szExeFile, pe->th32ProcessID, GetLastError());
		return false;
	}

	me.dwSize = sizeof(MODULEENTRY32);
	if (!Module32First(snapshot, &me)) {
		printf("%S (%d): Verification Failed: No Modules: %d\n", pe->szExeFile, pe->th32ProcessID, GetLastError());
		goto out_close;
	}

	// First module is the executable, and this is how we get the full path:
	if (log_name)
		printf("Target process found (%i): %S\n", pe->th32ProcessID, me.szExePath);
	wcscpy_s(exe_path, MAX_PATH, me.szExePath);

	rc = false;
	while (Module32Next(snapshot, &me)) {
		if (_wcsicmp(me.szModule, basename))
			continue;

		if (!_wcsicmp(me.szExePath, module)) {
			if (!pids.count(pe->th32ProcessID)) {
				printf("%d: 3DMigoto loaded :)\n", pe->th32ProcessID);
				pids.insert(pe->th32ProcessID);
			}
			rc = true;
		} else {
			wcscpy_s(mod_path, MAX_PATH, me.szExePath);
			wcsrchr(exe_path, L'\\')[1] = '\0';
			wcsrchr(mod_path, L'\\')[1] = '\0';
			if (!_wcsicmp(exe_path, mod_path)) {
				printf("\n\n\n"
				       "WARNING: Found a second copy of 3DMigoto loaded from the game directory:\n"
				       "%S\n"
				       "This may crash - please remove the copy in the game directory and try again\n\n\n",
				       me.szExePath);
			}
		}
	}

out_close:
	CloseHandle(snapshot);
	return rc;
}


static bool check_for_running_target(LPCWSTR target, LPCWSTR module)
{
	// https://docs.microsoft.com/en-us/windows/desktop/ToolHelp/taking-a-snapshot-and-viewing-processes
	HANDLE snapshot;
	PROCESSENTRY32 pe;
	bool rc = false;
	const wchar_t *basename = wcsrchr(target, '\\');
	static std::set<DWORD> pids;

	if (basename)
		basename++;
	else
		basename = target;

	snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snapshot == INVALID_HANDLE_VALUE) {
		printf("Check Failed: Invalid Handle: %d\n", GetLastError());
		return false;
	}

	pe.dwSize = sizeof(PROCESSENTRY32);
	if (!Process32First(snapshot, &pe)) {
		printf("Check Failed: No Processes: %d\n", GetLastError());
		goto out_close;
	}

	do {
		if (_wcsicmp(pe.szExeFile, basename))
			continue;

		rc = verify_injection(&pe, module, !pids.count(pe.th32ProcessID)) || rc;
		pids.insert(pe.th32ProcessID);

	} while (Process32Next(snapshot, &pe));

out_close:
	CloseHandle(snapshot);
	return rc;
}


// ----------------------------------------------------------------------------
// Setups global Windows Hook for target library
// Note:Make sure to remove hook with UnhookLibrary afterfards!
//
// Error codes:
// 100 - Another instance of 3DMigotoLoader is running
// 200 - Failed to load provided library
// 300 - Library is missing expected entry point
// 400 - Failed to setup windows hook

DLL_EXPORT int HookLibrary(LPCWSTR module_path, HHOOK *hook, HANDLE *mutex)
{
	FARPROC fn;
	//wchar_t module_full_path[MAX_PATH];

	//printf("Hooking %S", module_path);

	*mutex = CreateMutexA(0, FALSE, "Local\\3DMigotoLoader");
	if (GetLastError() == ERROR_ALREADY_EXISTS) {
		return 100;
	}

	//GetModuleFileName(NULL, module_full_path, MAX_PATH);
	//printf("WorkDir %S\n\n", module_full_path);

	// For relative path:
	//SetDllDirectory(L"SUBDIR");
	//module = LoadLibraryExW(L"d3d11.dll", NULL, LOAD_LIBRARY_SEARCH_USER_DIRS | LOAD_LIBRARY_SEARCH_SYSTEM32);

	*module = LoadLibraryExW(module_path, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
	if (!*module) {
		//printf("Error loading dll: %d/n", GetLastError());
		return 200;
	}

	//GetModuleFileName(module, module_full_path, MAX_PATH);
	//printf("Loaded %S\n\n", module_full_path);

	// Check if dll has CBTProc callback
	fn = GetProcAddress(*module, "CBTProc");
	if (!fn) {
		return 300;
	}

	// Setup hook for loaded dll
	*hook = SetWindowsHookEx(WH_CBT, (HOOKPROC)fn, *module, 0);
	if (!*hook) {
		return 400;
	}

	return EXIT_SUCCESS;
}


// ----------------------------------------------------------------------------
// Waits for given process to spawn (or untill timeout) and checks if module with given path was injected into it
DLL_EXPORT int WaitForInjection(LPCWSTR module_path, LPCWSTR target_process, int timeout = 10)
{
	for (int seconds = 0; seconds < +timeout; seconds++) {
		if (check_for_running_target(target_process, module_path))
			return EXIT_SUCCESS;
		Sleep(1000);
	}
	return EXIT_FAILURE;
}


// ----------------------------------------------------------------------------
// Removes installed hook for given handle and removes the Local\\3DMigotoLoader mutex
DLL_EXPORT int UnhookLibrary(HHOOK *hook, HANDLE* mutex)
{
	CloseHandle(*mutex);
	if (UnhookWindowsHookEx(*hook)) {
		if (FreeLibrary(*module)) {
			return EXIT_SUCCESS;
		}
	}
	return EXIT_FAILURE;
}


// ----------------------------------------------------------------------------
// Launches target exe file with provided args and working directory
DLL_EXPORT int StartProcess(LPCWSTR exe_path, LPCWSTR work_dir, LPCWSTR start_args)
{
	HINSTANCE result = ShellExecute(NULL, NULL, exe_path, start_args, work_dir, SW_SHOWNORMAL);
	if ((INT_PTR)result <= 32) {
		return (INT_PTR)result;
	}
	return EXIT_SUCCESS;
}



// ----------------------------------------------------------------------------
// Injects a dll into a process using WriteProcessMemory
//
// Error codes:
// 100 - Process {pid} not found
// 200 - Failed to allocate memory
// 300 - Failed to write DLL path to process memory
// 400 - Failed to create injection thread
// 500 - Injection thread timed out
// 600 - DLL injection failed

DLL_EXPORT int Inject(DWORD pid, LPCWSTR module_path) {
	HANDLE process, thread = NULL;
	LPVOID memory = NULL;
	DWORD wait_code = NULL;
	LPDWORD thread_code = NULL;
	int exit_code = EXIT_SUCCESS;
	process = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
	if (!process) {
		exit_code = 100;
		goto cleanup;
	}

	// Length of module path in bytes
	// Path is a wide string so the length must be multiplied by the size of a wide character
	size_t module_path_length = (wcslen(module_path) + 1) * sizeof(wchar_t);

	// Allocate memory to hold the module path
	memory = VirtualAllocEx(process, NULL, module_path_length, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	if (!memory) {
		exit_code = 200;
		goto cleanup;
	}

	// Write module path to allocated memory
	if (!WriteProcessMemory(process, memory, module_path, module_path_length, NULL)) {
		exit_code = 300;
		goto cleanup;
	}

	// Create a thread in the process to load the module from path in memory
	thread = CreateRemoteThread(process, NULL, 0,
		(LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandle(L"kernel32.dll"), "LoadLibraryW"), memory, 0, NULL);
	if (!thread) {
		exit_code = 400;
		goto cleanup;
	}

	wait_code = WaitForSingleObject(thread, 5000);
	switch (wait_code){
		case WAIT_TIMEOUT:
			exit_code = 500;
			goto cleanup;
		case WAIT_FAILED:
			exit_code = 600;
			goto cleanup;
	}

	cleanup:
		if (thread) CloseHandle(thread);
		if (memory) VirtualFreeEx(process, memory, 0, MEM_RELEASE);
		if (process) CloseHandle(process);
		return exit_code;
}


DLL_EXPORT BOOL APIENTRY DllMain(
	_In_  HINSTANCE hinstDLL,
	_In_  DWORD fdwReason,
	_In_  LPVOID lpvReserved)
{
	switch (fdwReason)
	{
		case DLL_PROCESS_ATTACH:
			return true;

		case DLL_PROCESS_DETACH:
			break;

		case DLL_THREAD_ATTACH:
			break;

		case DLL_THREAD_DETACH:
			break;
	}
	return true;
}
