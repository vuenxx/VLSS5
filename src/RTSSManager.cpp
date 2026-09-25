#include "RTSSManager.h"
#include "ConfigManager.h"
#include "Common.h"
#include <windows.h>
#include <shlwapi.h>
#include <cstdint>

#pragma comment(lib, "shlwapi.lib")

namespace
{
    // RTSS_SHARED_MEMORY'nin (RTSSSharedMemory.h, RTSS SDK) sabit basligi -- v2.x icin
    // gecerli, alan sirasi/boyutlari RTSS'in kendi tanimiyla birebir aynidir.
    #pragma pack(push, 8)
    struct RtssSharedMemoryHeader
    {
        DWORD dwSignature;
        DWORD dwVersion;
        DWORD dwAppEntrySize;
        DWORD dwAppArrOffset;
        DWORD dwAppArrSize;
        DWORD dwOSDEntrySize;
        DWORD dwOSDArrOffset;
        DWORD dwOSDArrSize;
        DWORD dwOSDFrame;
    };

    // Gercek RTSS_SHARED_MEMORY_APP_ENTRY cok daha buyuk (dwStatFrameTimeBuf[1024] dahil
    // bircok istatistik alani var) -- ama biz sadece basindaki alanlara ihtiyac duyuyoruz.
    // Diziler arasi ATLAMA icin bu struct'in sizeof'u DEGIL, RTSS'in bize verdigi
    // dwAppEntrySize kullaniliyor (bkz. RTSSManager::GetLiveFps) -- bu yuzden kirpilmis
    // olmasi guvenli, ileride RTSS yeni alanlar eklese de bozulmaz.
    struct RtssAppEntryHeader
    {
        DWORD dwProcessID;
        char  szName[MAX_PATH];
        DWORD dwFlags;
        DWORD dwTime0;
        DWORD dwTime1;
        DWORD dwFrames;
        DWORD dwFrameTime;
    };
    #pragma pack(pop)

    // 'RTSS' multi-char literalinin MSVC'deki degeri (ilk karakter en yuksek bayt) --
    // RTSS'in kendi header'i imzayi 'RTSS' olarak tanimliyor.
    constexpr DWORD kRtssSignature = ('R' << 24) | ('T' << 16) | ('S' << 8) | 'S';
}

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

// -----------------------------------------------------------------------
// EnsureSharedMemoryMapped / GetLiveFps
//   bkz. RTSSManager.h -- RTSS'in "RTSSSharedMemoryV2" paylasimli bellegini okuyup
//   ekran-yakalama katmanindan tamamen bagimsiz, gercek oyun kare hizini dondurur.
// -----------------------------------------------------------------------
bool RTSSManager::EnsureSharedMemoryMapped()
{
    if (m_rtssView) return true;

    // RTSS kapaliyken/henuz acilmamiskan bu basarisiz olur -- normal, her cagrida
    // tekrar denenir (RTSS calisirken acilip kapanabilir; sabit bir "basarisiz oldu,
    // bir daha deneme" bayragi TUTMUYORUZ ki kullanici RTSS'i sonradan acarsa
    // yeniden baglanabilsin).
    HANDLE hMap = OpenFileMappingA(FILE_MAP_READ, FALSE, "RTSSSharedMemoryV2");
    if (!hMap) return false;

    void* view = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
    if (!view)
    {
        CloseHandle(hMap);
        return false;
    }

    m_rtssMapping = hMap;
    m_rtssView    = view;
    return true;
}

int RTSSManager::GetLiveFps(unsigned long processId)
{
    if (!EnsureSharedMemoryMapped()) return -1;

    const auto* header = static_cast<const RtssSharedMemoryHeader*>(m_rtssView);
    if (header->dwSignature != kRtssSignature || (header->dwVersion & 0xFFFF0000u) < 0x00020000u)
    {
        // RTSS kapaniyor/yeniden baslatiliyor olabilir -- haritalamayi birak, bir
        // sonraki cagrida EnsureSharedMemoryMapped zaten m_rtssView dolu oldugundan
        // yeniden acmayacak; RTSS gercekten kapandiysa bu kontrol -1 dondurmeye
        // devam eder (zararsiz, sadece capture-tabanli yaklasik degere dusulur).
        return -1;
    }

    const auto* base   = static_cast<const uint8_t*>(m_rtssView);
    const auto* appArr = base + header->dwAppArrOffset;

    for (DWORD i = 0; i < header->dwAppArrSize; ++i)
    {
        const auto* entry = reinterpret_cast<const RtssAppEntryHeader*>(
            appArr + static_cast<size_t>(i) * header->dwAppEntrySize);

        if (entry->dwProcessID == 0) break; // bos slotlardan sonrasi da bostur
        if (entry->dwProcessID != processId) continue;

        if (entry->dwTime1 > entry->dwTime0 && entry->dwFrames > 0)
        {
            const double fps = 1000.0 * static_cast<double>(entry->dwFrames) /
                                static_cast<double>(entry->dwTime1 - entry->dwTime0);
            return static_cast<int>(fps + 0.5);
        }
        return -1; // RTSS bu process'i hook'lamis ama henuz olcum birikmemis
    }

    return -1; // RTSS bu process'i hic hook'lamamis (RTSS kapali ya da profil devre disi)
}
