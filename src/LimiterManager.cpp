#include "LimiterManager.h"
#include "RTSSManager.h"
#include <shlwapi.h>

#pragma comment(lib, "shlwapi.lib")

using vlss5::limiter::SharedState;
using vlss5::limiter::kProtocolVersion;
using vlss5::limiter::kMappingPrefix;

LimiterManager& LimiterManager::Get()
{
    static LimiterManager instance;
    return instance;
}

static std::wstring ExeSiblingPath(const wchar_t* name)
{
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    PathRemoveFileSpecW(path);
    PathCombineW(path, path, name);
    return path;
}

std::wstring LimiterManager::LimiterDllPath()    const { return ExeSiblingPath(L"vlss5_limiter.dll"); }
std::wstring LimiterManager::LimiterDll32Path()  const { return ExeSiblingPath(L"vlss5_limiter32.dll"); }
std::wstring LimiterManager::InjectorExe32Path() const { return ExeSiblingPath(L"vlss5_inject32.exe"); }

// 32-bit yardimciyi "vlss5_inject32.exe <pid> <dll32>" olarak calistirip bitmesini
// bekler. Enjeksiyon dizisini yardimci KENDI (32-bit) mimarisinde yapar; burada
// yalnizca surec baslatma + exit code var (enjeksiyon primitifi degil).
bool LimiterManager::LaunchInjector32(DWORD pid, const std::wstring& dll32Path)
{
    const std::wstring helper = InjectorExe32Path();
    if (GetFileAttributesW(helper.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        m_lastError = L"vlss5_inject32.exe bulunamadi (exe yaninda olmali)";
        return false;
    }

    std::wstring cmd = L"\"" + helper + L"\" " + std::to_wstring(pid) + L" \"" + dll32Path + L"\"";

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(helper.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
    {
        m_lastError = L"vlss5_inject32.exe baslatilamadi";
        return false;
    }

    // Yardimci enjeksiyonu baslatip hemen cikar (DLL icindeki hook kurulumu ayri
    // thread'de). 5 sn ust sinir yeterli; normalde <200 ms.
    WaitForSingleObject(pi.hProcess, 5000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    if (code != 0)
    {
        m_lastError = L"32-bit enjeksiyon yardimcisi hata dondu (kod=" + std::to_wstring(code) + L")";
        return false;
    }
    return true;
}

// ===========================================================================
// Oturum baslat
// ===========================================================================
bool LimiterManager::Begin(DWORD pid, const std::wstring& exeName)
{
    m_pid     = pid;
    m_exeName = exeName;
    m_lastError.clear();

    if (m_backend == Backend::Injection)
    {
        // Zaten AYNI hedefe enjekte edilmisse YENIDEN ENJEKTE ETME. DLL yuklu
        // kalir; sadece yeni limit oturumunu acariz. Ikinci kez LoadLibrary
        // DllMain'i tekrar calistirmaz (hook yeniden kurulmazdi) -- bu yuzden
        // reuse sart. (Ilk kalibrasyon bitince DLL yuklu kaldi; ikinci F2 burayi
        // vurur ve ayni hook'u tekrar kullanir.)
        if (m_injected && m_injectedPid == pid)
        {
            m_active = true;
            return true;
        }

        // Farkli bir hedefe enjekte edilmisse once eskiyi tamamen bosalt.
        if (m_injected)
            Shutdown();

        if (!InjectionBegin(pid)) // basarida m_injected=true, m_injectedPid=pid
            return false;

        m_active = true;
        return true;
    }

    // ---- RTSS arka uci ----
    m_rtssOriginalLimit = RTSSManager::Get().GetFramerateLimit(exeName);
    m_active = true;
    return true;
}

// ===========================================================================
// Limit uygula (canli)
// ===========================================================================
bool LimiterManager::ApplyLimit(int fps)
{
    if (!m_active) return false;
    if (fps < 0) fps = 0;

    if (m_backend == Backend::Injection)
    {
        if (!m_shared) return false;
        // DLL her karede okur; tek alan, tek yazar -- kilit gerekmez.
        m_shared->targetFps = fps;
        return true;
    }

    if (!RTSSManager::Get().SetFramerateLimit(m_exeName, fps))
    {
        m_lastError = L"RTSS profili yazilamadi";
        return false;
    }
    return true;
}

int LimiterManager::MeasuredFps() const
{
    if (m_backend == Backend::Injection && m_shared && m_shared->dllAttached)
        return m_shared->measuredFps;
    return -1; // RTSS olcum saglamaz; cagiran GetLiveFps kullanir
}

bool LimiterManager::IsBackendLive() const
{
    if (m_backend == Backend::Injection)
        return m_shared && m_shared->dllAttached != 0;
    return m_active; // RTSS: ayri surec, oturum acildiysa hazir sayilir
}

int LimiterManager::DllStatus() const
{
    if (m_backend == Backend::Injection && m_shared)
        return m_shared->dllStatus;
    return 0;
}

int LimiterManager::InstalledHooks() const
{
    return (m_backend == Backend::Injection && m_shared) ? (int)m_shared->installedHooks : 0;
}

int LimiterManager::LoadedModules() const
{
    return (m_backend == Backend::Injection && m_shared) ? (int)m_shared->loadedModules : 0;
}

int LimiterManager::PresentApi() const
{
    return (m_backend == Backend::Injection && m_shared) ? (int)m_shared->presentApi : 0;
}

// ===========================================================================
// Restore -- limit oturumunu bitir, enjeksiyonu (DLL'i) KORU
// ===========================================================================
void LimiterManager::Restore()
{
    if (m_backend == Backend::Injection)
    {
        // DLL'i sokme; sadece sinirsiza al. Bir sonraki kalibrasyon ayni hook'u
        // yeniden kullanacak. Tam teardown Shutdown()'da (StopOverlay).
        if (m_shared) m_shared->targetFps = vlss5::limiter::kFpsUnlimited;
    }
    else if (m_active)
    {
        RTSSManager::Get().SetFramerateLimit(m_exeName, m_rtssOriginalLimit);
    }
    m_active = false;
}

// ===========================================================================
// Shutdown -- enjeksiyon oturumunu TAMAMEN kapat (DLL uninject)
// ===========================================================================
void LimiterManager::Shutdown()
{
    if (m_backend == Backend::Injection)
    {
        InjectionRestore(); // shutdownSignal -> DLL kendini bosaltir; m_injected=false
    }
    else if (m_active)
    {
        RTSSManager::Get().SetFramerateLimit(m_exeName, m_rtssOriginalLimit);
    }

    m_active = false;
    m_pid = 0;
    m_exeName.clear();
}

// ===========================================================================
// Enjeksiyon arka uci
//
// Bu dosyada HAZIR olan kisimlar:
//   * Hedef PID'e gore adlandirilmis paylasimli bellegi (SharedState) olusturup
//     protokol surumu + varsayilanlari yazar.
//   * Hedef surece erisim tutamacini (OpenProcess) acar.
//   * Geri alma: targetFps=0 yazar, DLL'e kapanma isaretler, kaynaklari birakir.
//
// DOLDURULMASI GEREKEN tek yer: LoadRemoteModule() -- exe yanindaki
// vlss5_limiter.dll'i hedef surecin loader'ina yukleten standart, MSDN'de
// belgeli Win32 dizisi (hedef icinde bellek ayir, DLL yolunu yaz, uzaktan
// LoadLibraryW cagir, thread'i bekle, ayrilan bellegi birak). Doldurulana
// kadar enjeksiyon arka uci guvenle false doner ve kalibrasyon RTSS'e duser.
// ===========================================================================

// Exe yanindaki vlss5_limiter.dll'i `process` ile temsil edilen hedef surece
// yukler. Basarida true. Su an DOLDURULMAMIS bir uzanti noktasi: gerceklenene
// kadar enjeksiyon devre disidir.
static bool LoadRemoteModule(HANDLE process, const std::wstring& dllPath, std::wstring& err)
{
    const SIZE_T pathBytes = (dllPath.size() + 1) * sizeof(wchar_t);

    void* remote = VirtualAllocEx(process, nullptr, pathBytes,
                                  MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote)
    {
        err = L"Hedef surecte bellek ayrilamadi (VirtualAllocEx)";
        return false;
    }

    bool ok = false;
    HANDLE thread = nullptr;

    if (!WriteProcessMemory(process, remote, dllPath.c_str(), pathBytes, nullptr))
    {
        err = L"DLL yolu hedefe yazilamadi (WriteProcessMemory)";
        goto cleanup;
    }

    {
        auto loadLib = reinterpret_cast<LPTHREAD_START_ROUTINE>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
        if (!loadLib)
        {
            err = L"LoadLibraryW adresi alinamadi";
            goto cleanup;
        }

        thread = CreateRemoteThread(process, nullptr, 0, loadLib, remote, 0, nullptr);
        if (!thread)
        {
            err = L"Uzak thread olusturulamadi (CreateRemoteThread)";
            goto cleanup;
        }

        // INFINITE yerine timeout: hedefte LoadLibraryW (ya da DLL'in DllMain'i)
        // bir sekilde takilirsa VLSS5 thread'ini sonsuza kilitlememek icin.
        // Bizim DllMain hook kurulumunu ayri thread'e atip hemen donuyor, yani
        // 10 sn fazlasiyla yeterli.
        if (WaitForSingleObject(thread, 10000) != WAIT_OBJECT_0)
        {
            err = L"Enjeksiyon zaman asimina ugradi (LoadLibraryW donmedi)";
            goto cleanup;
        }

        // DIKKAT: basariyi GetExitCodeThread ile OLCMUYORUZ. x64'te thread cikis
        // kodu DWORD'dur ve LoadLibraryW'nin dondurdugu HMODULE'un yalnizca alt
        // 32 bitini tutar -- DLL yuklense bile 0 cikabilir (yanlis negatif).
        // Gercek dogrulama cagiranda yapilir: DLL kendisi SharedState.dllAttached'i
        // set eder, InjectionBegin onu bekler. Burada "thread tamamlandi" yeterli.
        ok = true;
    }

cleanup:
    if (thread) CloseHandle(thread);
    VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    return ok;
}

bool LimiterManager::InjectionBegin(DWORD pid)
{
    const std::wstring dllPath = LimiterDllPath();
    if (GetFileAttributesW(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        m_lastError = L"vlss5_limiter.dll bulunamadi (exe yaninda olmali)";
        return false;
    }

    // --- Paylasimli bellek ---
    wchar_t mapName[128] = {};
    swprintf_s(mapName, L"%s%u", kMappingPrefix, pid);

    m_mapFile = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                   0, sizeof(SharedState), mapName);
    if (!m_mapFile)
    {
        m_lastError = L"Paylasimli bellek olusturulamadi";
        return false;
    }

    m_shared = (SharedState*)MapViewOfFile(m_mapFile, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedState));
    if (!m_shared)
    {
        m_lastError = L"Paylasimli bellek eslenemedi";
        CloseHandle(m_mapFile); m_mapFile = nullptr;
        return false;
    }

    ZeroMemory(m_shared, sizeof(SharedState));
    m_shared->protocolVersion = kProtocolVersion;
    m_shared->targetFps       = vlss5::limiter::kFpsUnlimited;
    m_shared->waitMode        = (int32_t)vlss5::limiter::WaitMode::Hybrid;

    // --- Hedef surece erisim ---
    m_process = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE |
        PROCESS_VM_READ | PROCESS_QUERY_INFORMATION,
        FALSE, pid);
    if (!m_process)
    {
        m_lastError = L"Hedef surece erisilemedi (yonetici gerekebilir)";
        InjectionRestore();
        return false;
    }

    // --- Bit genisligi: hedefe gore enjeksiyon yolu ---
    // VLSS5 x64. 64-bit DLL, WOW64 (32-bit) surece YUKLENEMEZ; 32-bit hedefte
    // enjeksiyonu 32-bit yardimci (vlss5_inject32.exe) kendi mimarisinde yapar
    // ve 32-bit DLL'i (vlss5_limiter32.dll) yukler. Paylasimli bellek ve protokol
    // her iki mimaride ayni (SharedState isaretci icermez).
    BOOL targetWow64 = FALSE;
    IsWow64Process(m_process, &targetWow64);

    if (targetWow64)
    {
        const std::wstring dll32 = LimiterDll32Path();
        if (GetFileAttributesW(dll32.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            m_lastError = L"vlss5_limiter32.dll bulunamadi (exe yaninda olmali)";
            InjectionRestore();
            return false;
        }
        if (!LaunchInjector32(pid, dll32)) // m_lastError iceride set edilir
        {
            InjectionRestore();
            return false;
        }
    }
    else
    {
        // 64-bit hedef: uygulama ici dogrudan enjeksiyon.
        std::wstring loadErr;
        if (!LoadRemoteModule(m_process, dllPath, loadErr))
        {
            m_lastError = loadErr;
            InjectionRestore();
            return false;
        }
    }

    // DLL yukleme baslatildi. Hook kurulumu DLL'in ICINDE ayri bir thread'de
    // olur; burada BLOKLAMIYORUZ -- eski surum ~2 sn Sleep ile render thread'ini
    // donduruyordu (olculen 2000 ms kare boslugu). Cagiran (kalibrasyon)
    // IsBackendLive()'i kendi tikleri arasinda non-blocking yoklar; dllAttached
    // DLL hook'u kurunca 1 olur.
    m_injected    = true;
    m_injectedPid = pid;
    return true;
}

void LimiterManager::InjectionRestore()
{
    // Once sinirsiza al (oyun dusuk limitte takili kalmasin), sonra shutdownSignal
    // yaz: DLL'in izleme thread'i bunu gorup hook'u geri alir ve KENDINI BOSALTIR
    // (uninject). DLL kendi mapping tutamacini tuttugu icin, biz asagida view'i
    // unmap edip handle'i kapatsak da paylasimli bellek DLL ayrilana kadar yasar.
    if (m_shared)
    {
        m_shared->targetFps      = vlss5::limiter::kFpsUnlimited;
        m_shared->shutdownSignal = 1;
        UnmapViewOfFile(m_shared);
        m_shared = nullptr;
    }
    if (m_mapFile) { CloseHandle(m_mapFile); m_mapFile = nullptr; }
    if (m_process) { CloseHandle(m_process); m_process = nullptr; }

    m_injected    = false;
    m_injectedPid = 0;
}
