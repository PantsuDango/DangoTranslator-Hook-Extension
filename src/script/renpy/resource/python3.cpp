#include "python.h"

bool InsertRenpy3Hook()
{
    if (HMODULE module = GetModuleHandleW(L"python27.dll"))
    {
        return hookrenpy(module);
    }
    wchar_t pythonPattern[] = L"python3%d.dll";
    wchar_t libPythonPattern[] = L"libpython3.%d.dll";
    wchar_t moduleName[64] = {0};
    for (const wchar_t *pattern : {pythonPattern, libPythonPattern})
    {
        for (int minorVersion = 0; minorVersion <= 20; ++minorVersion)
        {
            swprintf_s(moduleName, pattern, minorVersion);
            if (HMODULE module = GetModuleHandleW(moduleName))
            {
                return hookrenpy(module);
            }
        }
    }
    return false;
}
