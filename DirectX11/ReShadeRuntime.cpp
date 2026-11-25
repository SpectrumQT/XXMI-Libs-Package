#include "ReShadeRuntime.hpp"

bool InitializeHooking(HMODULE module)
{
    if (!module)
        return false;

    // Prevent accidental reinitialization with another module.
    if (g_module_handle && g_module_handle != module)
        return false;

    g_module_handle = module;

    if (g_reshade_dll_path.empty())
        g_reshade_dll_path = get_module_path(module);

    return !g_reshade_dll_path.empty();
}

bool InitializeHooksLog()
{
    static std::once_flag once;
    static bool initialized = false;

    if (!g_module_handle || g_reshade_dll_path.empty())
        return false;

    std::call_once(once, []()
    {
        const auto log_directory = g_reshade_dll_path.parent_path();
        auto log_path = log_directory / L"hooks_log.txt";

        std::error_code ec;

        if (reshade::log::open_log_file(log_path, ec))
        {
            initialized = true;
            return;
        }

        auto last_error = ec;

        for (int i = 1; i <= 10; ++i)
        {
            std::error_code exists_ec;

            if (!std::filesystem::exists(log_path, exists_ec) || exists_ec)
                break;

            log_path = log_directory / (L"hooks_log" + std::to_wstring(i) + L".txt");

            if (reshade::log::open_log_file(log_path, ec))
            {
                initialized = true;
                return;
            }

            last_error = ec;
        }

        LogInfo("Failed to initialize hooks logging: %s\n", last_error.message().c_str());
    });

    return initialized;
}

bool InstallQueuedHooks()
{
    return reshade::hook::apply_queued_actions();
}