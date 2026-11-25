#pragma once

#include "dll_main.h"
#include <hook_manager.hpp>

#include "log.h"

bool InitializeHooking(HMODULE module);
bool InitializeHooksLog();

template <typename Fn, typename Hook>
bool QueueHook(HMODULE module, const char* name, Hook replacement, Fn* original)
{
    if (!module)
    {
        LogInfo("Failed to get module handle for %s\n", name);
        return false;
    }

    auto procedure = GetProcAddress(module, name);
    if (!procedure)
    {
        LogInfo("Failed to get address of %s\n", name);
        return false;
    }

    auto target = reinterpret_cast<reshade::hook::address>(procedure);
    auto hook = reinterpret_cast<reshade::hook::address>(replacement);

    if (!reshade::hooks::install(name, target, hook, true))
    {
        LogInfo("Failed to queue hook %s\n", name);
        return false;
    }

    *original = reshade::hooks::call<Fn>(replacement, target);
    return *original != nullptr;
}

bool InstallQueuedHooks();