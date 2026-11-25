#pragma once

#include "dll_log.hpp"

#include <Windows.h>

extern HINSTANCE g_module_handle;
extern std::filesystem::path g_reshade_dll_path;

std::filesystem::path get_module_path(HINSTANCE module);