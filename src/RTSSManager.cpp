#include "RTSSManager.h"
#include "ConfigManager.h"
#include <windows.h>
#include <shlwapi.h>

#pragma comment(lib, "shlwapi.lib")

RTSSManager& RTSSManager::Get()
{
    static RTSSManager instance;
    return instance;
}

std::wstring RTSSManager::GetProfilesDir() const
{
    std::wstring dir = ConfigManager::Get().Config().rtssDirectory;
    if (dir.empty())
    {
        return L"C:\\Program Files (x86)\\RivaTuner Statistics Server\\Profiles";
    }
    return dir;
}

std::vector<std::wstring> RTSSManager::GetProfiles()
{
    std::vector<std::wstring> profiles;
    std::wstring searchPath = GetProfilesDir() + L"\\*.cfg";

    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(searchPath.c_str(), &fd);
    if (hFind != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            {
                std::wstring name = fd.cFileName;
                // Sadece .cfg uzantısını kırp
                if (name.length() > 4)
                {
                    name = name.substr(0, name.length() - 4);
                }
                profiles.push_back(name);
            }
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    }
    return profiles;
}

bool RTSSManager::SetFramerateLimit(const std::wstring& exeName, int limit)
{
    std::wstring cfgPath = GetProfilesDir() + L"\\" + exeName + L".cfg";
    
    wchar_t limitStr[32];
    swprintf_s(limitStr, L"%d", limit);

    // [Framerate] Limit=X yaz
    BOOL res = WritePrivateProfileStringW(L"Framerate", L"Limit", limitStr, cfgPath.c_str());
    
    // Modern RTSS requires LimitDenominator=1 to apply integer limits correctly
    if (res)
    {
        WritePrivateProfileStringW(L"Framerate", L"LimitDenominator", L"1", cfgPath.c_str());
        NotifyRTSS();
    }
    
    return res != FALSE;
}

void RTSSManager::NotifyRTSS()
{
    // Notify RTSS to reload profiles dynamically using RTSSHooks64.dll
    std::wstring dllPath = GetProfilesDir() + L"\\..\\RTSSHooks64.dll";
    
    HMODULE hModule = LoadLibraryW(dllPath.c_str());
    if (hModule)
    {
        typedef void (*UPDATEPROFILES)();
        UPDATEPROFILES pUpdateProfiles = (UPDATEPROFILES)GetProcAddress(hModule, "UpdateProfiles");
        if (pUpdateProfiles)
        {
            pUpdateProfiles();
        }
        FreeLibrary(hModule);
    }
}
