#include "RTSSManager.h"
#include "ConfigManager.h"
#include "Common.h"
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
    
    // If the user selected the main RTSS folder instead of the Profiles folder, append it
    if (dir.find(L"Profiles") == std::wstring::npos)
    {
        dir += L"\\Profiles";
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

int RTSSManager::GetFramerateLimit(const std::wstring& exeName)
{
    if (exeName.empty()) return 0;
    std::wstring cfgPath = GetProfilesDir() + L"\\" + exeName + L".cfg";
    return static_cast<int>(
        GetPrivateProfileIntW(L"Framerate", L"Limit", 0, cfgPath.c_str()));
}

bool RTSSManager::SetFramerateLimit(const std::wstring& exeName, int limit)
{
    if (exeName.empty()) return false;

    // Negatif limit RTSS icin anlamsizdir ve profili bozar.
    if (limit < 0)
    {
        DLSS_Log("[RTSS] REDDEDILDI: negatif FPS limiti (%d) yazilmaya calisildi (%ls)",
                 limit, exeName.c_str());
        return false;
    }

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
    
    // ---- Yazma dogrulamasi ----
    // RTSS profilleri BELLEKTE tutar ve kendi kopyasini diske geri yazar. Bizim
    // WritePrivateProfileString cagrimiz sessizce ezilebiliyor: gercek bir oturumda
    // kalibrasyon 33 yazdi, dosyada once 40 sonra 34 gorundu. Yani .cfg tek basina
    // guvenilir bir kanal degil. Sessiz basarisizlik yerine gorunur uyari uretiyoruz.
    if (res)
    {
        const int readBack = static_cast<int>(
            GetPrivateProfileIntW(L"Framerate", L"Limit", -1, cfgPath.c_str()));
        if (readBack != limit)
        {
            DLSS_Log("[RTSS] UYARI: yazilan limit (%d) profilde tutmadi, okunan=%d (%ls). "
                     "RTSS profili bellekten geri yaziyor olabilir; RTSS penceresi acikken "
                     "kalibrasyon guvenilir degildir.", limit, readBack, exeName.c_str());
        }
    }

    return res != FALSE;
}

void RTSSManager::NotifyRTSS()
{
    // ONEMLI: Bu fonksiyon RENDER THREAD'inden cagriliyor.
    //
    // Eskiden her cagrida RTSSHooks64.dll LoadLibraryW + FreeLibrary ediliyordu.
    // RTSSHooks64.dll bir hooking kutuphanesidir; sureci ice/disari almak
    // DLL_PROCESS_ATTACH/DETACH yollarini isletir, loader kilidini tutar ve grafik
    // yiginini gecici olarak kararsizlastirir.
    //
    // Olculen sonuc: her limit yazma isleminden ~140 ms sonra tek bir dev Present()
    // stall'i. Gercek log (2026-09-15 19:10):
    //     [19:10:39.695] [Calib] Limit uygulandi: 33  ->  [19:10:39.837] present=141.24 ms
    //     [19:10:45.701] [Calib] Limit uygulandi: 35  ->  [19:10:45.846] present=128.75 ms
    //     [19:10:51.718] [Calib] Limit uygulandi: 36  ->  [19:10:51.863] present=143.79 ms
    // Bu stall'lar kalibrasyon olcumlerini de zehirliyordu.
    //
    // Modul artik bir kez yuklenip surec omru boyunca tutuluyor. FreeLibrary YOK:
    // bir hooking kutuphanesini tekrar tekrar bosaltmak zaten guvenli degil.
    typedef void (*UPDATEPROFILES)();

    static HMODULE        s_hModule         = nullptr;
    static UPDATEPROFILES s_pUpdateProfiles = nullptr;
    static bool           s_loadAttempted   = false;

    if (!s_loadAttempted)
    {
        s_loadAttempted = true;

        std::wstring dllPath = GetProfilesDir() + L"\\..\\RTSSHooks64.dll";
        s_hModule = LoadLibraryW(dllPath.c_str());
        if (s_hModule)
        {
            s_pUpdateProfiles = (UPDATEPROFILES)GetProcAddress(s_hModule, "UpdateProfiles");
            DLSS_Log("[RTSS] RTSSHooks64.dll kalici olarak yuklendi | UpdateProfiles=%p",
                     s_pUpdateProfiles);
        }
        else
        {
            DLSS_Log("[RTSS] UYARI: RTSSHooks64.dll yuklenemedi (%ls). "
                     "Profil degisiklikleri RTSS'e bildirilemeyecek.", dllPath.c_str());
        }
    }

    if (s_pUpdateProfiles) s_pUpdateProfiles();
}
