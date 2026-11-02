// ===========================================================================
// VLSS5 Dahili FPS Limiter -- Hook DLL (vlss5_limiter.dll)
//
// NE YAPAR: Hedef oyunun icine yuklendiginde Present cagrisini sarmalar --
// IDXGISwapChain::Present (DirectX 10/11/12), IDirect3DDevice9::Present (D3D9)
// ve IDirect3DDevice8::Present (D3D8). Her karede, ana uygulamanin paylasimli
// bellege yazdigi targetFps'e gore bir sonraki Present'i geciktirir (sleep +
// spin). RTSS / Special K / MSI Afterburner ile ayni teknik; amac oyunun kare
// uretim hizini VLSS5'in yakalama boru hattinin isleyebildigi hiza indirmek.
//
// NE YAPMAZ: Oyun bellegini okumaz/yazmaz, girdi enjekte etmez, hicbir oyun
// mantigini degistirmez. Yalnizca kendi surecindeki DXGI Present cagrisinin
// zamanlamasini etkiler. Gizlenmeye calismaz -- modul adi ve davranisi acik.
//
// GUVENLIK: FPS sinirlama hicbir rekabet avantaji saglamaz (aksine FPS'i
// DUSURUR). Yine de cekirdek-seviye anti-cheat'ler her turlu modul yuklemesini
// riskli gorebilir; bu yuzden ana uygulama tarafinda opt-in ve uyarilidir.
// ===========================================================================

#include <windows.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <d3d11.h>
#include <d3d9.h>
#include <cstdio>
#include "vlss5_limiter_shared.h"

// NOT: d3d9.lib'e LINK ETMIYORUZ (yukleme-zamani bagimliligi olusturmasin diye).
// Direct3DCreate9'u, yalnizca oyun d3d9.dll'i zaten yuklemisse GetProcAddress ile
// cozuyoruz -- boylece D3D9 kullanmayan oyunlara d3d9.dll zorla yuklenmez.

using namespace vlss5::limiter;

// ---------------------------------------------------------------------------
// Durum
// ---------------------------------------------------------------------------
namespace
{
    HMODULE          g_selfModule   = nullptr;
    HANDLE           g_mapFile       = nullptr;
    SharedState*     g_shared        = nullptr;
    volatile LONG    g_shuttingDown  = 0;

    // DXGI (D3D10/11/12) IDXGISwapChain::Present (vtable[8]) -- yamalanmadan onceki
    // orijinal fonksiyon. Vtable slotu atomik swap ile hook'lanir; sarmalayici
    // isini bitirince bunu cagirir.
    using PresentFn = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
    PresentFn        g_realPresent   = nullptr;

    // IDXGISwapChain1::Present1 (vtable[22]) -- flip-model oyunlarin cogu bunu cagirir.
    using Present1Fn = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
    Present1Fn       g_realPresent1  = nullptr;

    // D3D9 IDirect3DDevice9::Present vtable[17] -- eski oyunlar icin.
    using Present9Fn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
    Present9Fn       g_realPresent9  = nullptr;

    // D3D8 IDirect3DDevice8::Present vtable[15] -- cok eski (2001-2004) oyunlar.
    // d3d8.h modern SDK'da yok; arayuzleri elle, ham vtable index'leriyle cagiriyoruz.
    using Present8Fn = HRESULT (STDMETHODCALLTYPE*)(void*, const RECT*, const RECT*, HWND, const RGNDATA*);
    Present8Fn       g_realPresent8  = nullptr;

    // Basariyla yamalanan vtable slotu sayisi (geri alma/tani icin).
    int              g_hookCount     = 0;

    // Zamanlama durumu.
    LARGE_INTEGER    g_qpcFreq       = {};
    LONGLONG         g_nextPresentQpc = 0;         // bir sonraki Present'in en erken izinli ani (QPC)

    // Telemetri.
    LONGLONG         g_fpsWindowStart = 0;
    uint64_t         g_fpsWindowCount = 0;
    uint64_t         g_heartbeatLast  = 0;
}

// ---------------------------------------------------------------------------
// Teshis logu (oyun surecinin icinden) -- %TEMP%\vlss5_limiter_dbg.log
// Oyun cokunce de son satirlar kalsin diye her yazimdan sonra flush edilir.
//
// VARSAYILAN KAPALI (release): hicbir sey yazmaz, sifir maliyet. Yeni bir oyun
// hook'lanamadiginda/cokdugunde teshis icin 1 yapip yeniden derle; log su yolda
// olusur: %TEMP%\vlss5_limiter_dbg.log
// ---------------------------------------------------------------------------
#define VLSS5_LIMITER_DBG 0

#if VLSS5_LIMITER_DBG
static void DbgLog(const char* fmt, ...)
{
    static CRITICAL_SECTION cs;
    static bool csInit = false;
    static FILE* f = nullptr;
    static bool opened = false;

    if (!csInit) { InitializeCriticalSection(&cs); csInit = true; }
    EnterCriticalSection(&cs);

    if (!opened)
    {
        opened = true;
        wchar_t dir[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, dir);
        wchar_t path[MAX_PATH] = {};
        swprintf_s(path, L"%svlss5_limiter_dbg.log", dir);
        if (_wfopen_s(&f, path, L"w") != 0) f = nullptr;
        if (f) { fprintf(f, "=== VLSS5 limiter DLL dbg (pid=%u) ===\n", GetCurrentProcessId()); fflush(f); }
    }

    if (f)
    {
        char buf[512];
        va_list a; va_start(a, fmt);
        vsnprintf(buf, sizeof(buf), fmt, a);
        va_end(a);
        SYSTEMTIME st; GetLocalTime(&st);
        fprintf(f, "[%02d:%02d:%02d.%03d] %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, buf);
        fflush(f);
    }
    LeaveCriticalSection(&cs);
}
#else
static inline void DbgLog(const char*, ...) {}   // release: no-op (derleyici optimize eder)
#endif

// ---------------------------------------------------------------------------
// Yardimcilar
// ---------------------------------------------------------------------------
static LONGLONG NowQpc()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

static double QpcToMs(LONGLONG ticks)
{
    return (1000.0 * (double)ticks) / (double)g_qpcFreq.QuadPart;
}

static LONGLONG MsToQpc(double ms)
{
    return (LONGLONG)((ms / 1000.0) * (double)g_qpcFreq.QuadPart);
}

// Paylasimli bellegi (kendi PID'imize gore adlandirilmis) ac. Ana uygulama
// enjeksiyondan ONCE olusturdugu icin burada yalnizca OpenFileMapping yeter.
static bool OpenSharedState()
{
    wchar_t name[128] = {};
    swprintf_s(name, L"%s%u", kMappingPrefix, GetCurrentProcessId());

    g_mapFile = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
    if (!g_mapFile) return false;

    g_shared = (SharedState*)MapViewOfFile(g_mapFile, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedState));
    if (!g_shared)
    {
        CloseHandle(g_mapFile);
        g_mapFile = nullptr;
        return false;
    }

    // Protokol uyumsuzsa cekiliriz: yanlis yorumlanmis bir struct'tan iyi.
    if (g_shared->protocolVersion != kProtocolVersion)
    {
        UnmapViewOfFile(g_shared); g_shared = nullptr;
        CloseHandle(g_mapFile);    g_mapFile = nullptr;
        return false;
    }
    g_shared->dllStatus = kStatusSharedOpened;
    return true;
}

// ---------------------------------------------------------------------------
// Kare hizi sinirlama -- Present oncesi bekleme
//
// Hedef kare araligi = 1000 / targetFps ms. Bir onceki izinli Present aninin
// uzerine araligi ekleyerek bir sonraki hedefi yuruturuz (sabit ritim).
// Cok gecikirsek (oyun duraksamasi, alt+tab) hedefi "simdi"ye sabitleyip
// birikmis borcu atariz -- aksi halde oyun hizlanarak borcu kapatmaya calisir.
// ---------------------------------------------------------------------------
static void PaceFrame()
{
    if (!g_shared) return;

    const int32_t target = g_shared->targetFps;
    const int32_t mode   = g_shared->waitMode;

    // Sinirsiz veya kapali: bekleme yok, ritmi de sifirla ki tekrar acildiginda
    // birikmis hedefle oyunu kilitlemeyelim.
    if (target <= 0)
    {
        g_nextPresentQpc = 0;
        return;
    }

    const double frameMs  = 1000.0 / (double)target;
    const LONGLONG frameT = MsToQpc(frameMs);
    const LONGLONG now    = NowQpc();

    if (g_nextPresentQpc == 0)
        g_nextPresentQpc = now; // ilk kare: hemen gecir, ritmi buradan baslat

    LONGLONG deadline = g_nextPresentQpc;

    // Cok geride kaldiysak (>2 kare) borcu affet.
    if (now - deadline > 2 * frameT)
        deadline = now;

    // Hibrit bekleme: hedefin ~1.5 ms altina kadar uyu, kalani spin'le kapat.
    // Saf Sleep modunda spin yok; saf Spin modunda hic uyumayiz.
    const LONGLONG spinMarginT = (mode == (int32_t)WaitMode::Sleep) ? 0 : MsToQpc(1.5);

    for (;;)
    {
        const LONGLONG cur = NowQpc();
        if (cur >= deadline) break;

        const LONGLONG remaining = deadline - cur;
        if (mode != (int32_t)WaitMode::Spin && remaining > spinMarginT)
        {
            // Uyunacak sure (son payi birak). timeBeginPeriod ana uygulamada
            // zaten 1 ms'e cekili; yine de kaba Sleep(1) yeterli granularite.
            Sleep(1);
        }
        else
        {
            YieldProcessor(); // kisa busy-wait
        }
        if (InterlockedCompareExchange(&g_shuttingDown, 0, 0)) break;
    }

    g_nextPresentQpc = deadline + frameT;
}

// Telemetriyi (olculen FPS, heartbeat, present sayaci) paylasimli bellege yaz.
static void UpdateTelemetry()
{
    if (!g_shared) return;

    ++g_fpsWindowCount;
    g_shared->presentCount++;

    const LONGLONG now = NowQpc();
    if (g_fpsWindowStart == 0) g_fpsWindowStart = now;

    const double elapsedMs = QpcToMs(now - g_fpsWindowStart);
    if (elapsedMs >= 500.0)
    {
        g_shared->measuredFps = (int32_t)((g_fpsWindowCount * 1000.0) / elapsedMs + 0.5);
        g_fpsWindowCount = 0;
        g_fpsWindowStart = now;
        g_shared->heartbeat = ++g_heartbeatLast;
    }
}

// ---------------------------------------------------------------------------
// Present sarmalayici
// ---------------------------------------------------------------------------
static HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* sc, UINT syncInterval, UINT flags)
{
    static LONG once = 0;
    if (InterlockedExchange(&once, 1) == 0) DbgLog("HookedPresent ILK kez atesledi (flags=0x%X)", flags);
    if (!InterlockedCompareExchange(&g_shuttingDown, 0, 0))
    {
        // DXGI_PRESENT_TEST sadece durum sorgusudur, gercek bir sunum degil --
        // onu geciktirmek anlamsiz ve zamanlamayi bozar.
        if ((flags & DXGI_PRESENT_TEST) == 0)
        {
            if (g_shared) g_shared->presentApi = 11; // oyun DXGI kullaniyor
            PaceFrame();
            UpdateTelemetry();
        }
    }
    return g_realPresent(sc, syncInterval, flags);
}

// IDXGISwapChain1::Present1 sarmalayici -- flip-model DXGI oyunlari icin.
static HRESULT STDMETHODCALLTYPE HookedPresent1(IDXGISwapChain1* sc, UINT syncInterval,
                                                UINT flags, const DXGI_PRESENT_PARAMETERS* pp)
{
    static LONG once = 0;
    if (InterlockedExchange(&once, 1) == 0) DbgLog("HookedPresent1 ILK kez atesledi (flags=0x%X)", flags);
    if (!InterlockedCompareExchange(&g_shuttingDown, 0, 0))
    {
        if ((flags & DXGI_PRESENT_TEST) == 0)
        {
            if (g_shared) g_shared->presentApi = 11;
            PaceFrame();
            UpdateTelemetry();
        }
    }
    return g_realPresent1(sc, syncInterval, flags, pp);
}

// D3D9 Present sarmalayici -- eski (DirectX 9) oyunlar icin. Mantik ayni:
// gercek Present'ten once PaceFrame ile kareyi geciktir.
static HRESULT STDMETHODCALLTYPE HookedPresent9(IDirect3DDevice9* dev, const RECT* src,
                                                const RECT* dest, HWND hWnd, const RGNDATA* dirty)
{
    if (!InterlockedCompareExchange(&g_shuttingDown, 0, 0))
    {
        if (g_shared) g_shared->presentApi = 9; // oyun D3D9 kullaniyor
        PaceFrame();
        UpdateTelemetry();
    }
    return g_realPresent9(dev, src, dest, hWnd, dirty);
}

// D3D8 Present sarmalayici -- cok eski oyunlar (DirectX 8). Imza D3D9 ile ayni.
static HRESULT STDMETHODCALLTYPE HookedPresent8(void* dev, const RECT* src,
                                                const RECT* dest, HWND hWnd, const RGNDATA* dirty)
{
    if (!InterlockedCompareExchange(&g_shuttingDown, 0, 0))
    {
        if (g_shared) g_shared->presentApi = 8; // oyun D3D8 kullaniyor
        PaceFrame();
        UpdateTelemetry();
    }
    return g_realPresent8(dev, src, dest, hWnd, dirty);
}

// ---------------------------------------------------------------------------
// Vtable swap tabanli hook kurulumu (DXGI + D3D9 + D3D8)
//
// Gizli bir pencere + GECICI bir cihaz (D3D11/D3D9/D3D8) olustururuz; tek amaci
// ilgili COM arayuzunun vtable'indeki Present slotunun ADRESINI okumak. vtable
// COM sinifi genelinde PAYLASILIR: dummy'nin nesnesi ile oyunun nesnesi AYNI
// SINIFTANSA ayni vtable dizisini paylasir, yani dummy'nin Present slotunu
// yamalamak oyunun gercek nesnesini de yamalar.
//
// NEDEN inline (MinHook) DEGIL: Calisan oyuna GEC enjeksiyonda, inline patch
// Present fonksiyonunun ilk byte'larini degistirir ve bunu yaparken tum thread'leri
// askiya alir. Oyun saniyede 60+ Present cagirdigi icin render thread'i tam o anda
// Present'in ICINDE olabilir; kod degisince uyandiginda bozuk koda dusup coker
// (olculen: PES, detour hic ateslenmeden silent crash). Vtable swap ise tek bir
// pointer'i atomik yazar: kod degismez, thread askiya alinmaz, o an Present icindeki
// thread guvenle biter, bir SONRAKI cagri sarmalayiciya gider. RTSS/ReShade de
// canli oyunda bu teknigi kullanir.
// ---------------------------------------------------------------------------

// Yamalanacak vtable slotlarinin defteri (DXGI Present/Present1, D3D9 ve D3D8
// Present -- hepsi ayni defterde). slot=vtable girisi, orig=orijinal fonksiyon,
// hookFn=bizim sarmalayici.
namespace
{
    struct VtablePatch { void** slot; void* orig; void* hookFn; };
    VtablePatch g_vtablePatches[8] = {};
    int         g_vtablePatchCount = 0;
}

// Bir vtable slotunu yamaya KAYDET (henuz YAZMA). Orijinali hemen okuyup outOrig'e
// ve deftere yazar; gercek atomik yazim daha sonra ApplyVtablePatches ile topluca
// yapilir. KRITIK: Boylece tum dummy cihaz olusturma/release islemleri HICBIR hook
// aktif DEGILKEN biter; aksi halde (DXGI hook aktifken D3D9/D3D8 dummy cihazi
// olusturmak) DXGI'nin surec-geneli kilidinde DEADLOCK olur ve oyun donar
// (olculen: CS2D d3d9.dll yukluyken dondu).
static void RecordVtablePatch(void** slot, void* hookFn, void** outOrig)
{
    for (int i = 0; i < g_vtablePatchCount; ++i)
        if (g_vtablePatches[i].slot == slot) return; // zaten kayitli (ayni vtable)

    void* cur = *slot;                        // sadece OKUMA -- yazma yok, deadlock yok
    if (cur == hookFn) return;                // zaten bizim hook
    if (outOrig && !*outOrig) *outOrig = cur; // gercek orijinali yakala
    if (g_vtablePatchCount < 8)
    {
        g_vtablePatches[g_vtablePatchCount++] = { slot, cur, hookFn };
        ++g_hookCount;
    }
}

// Kaydedilmis tum vtable yamalarini atomik olarak UYGULA. Bu noktada tum dummy
// cihazlar olusturulup serbest birakilmistir; hicbir cihaz olusturma/release ile
// yarismayiz. Her slot tek bir hizali pointer yazimiyla degisir (atomik) --
// kod degismez, thread askiya alinmaz.
static void ApplyVtablePatches()
{
    for (int i = 0; i < g_vtablePatchCount; ++i)
    {
        void** slot = g_vtablePatches[i].slot;
        DWORD op = 0;
        if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &op))
        {
            if (*slot != g_vtablePatches[i].hookFn)
                *slot = g_vtablePatches[i].hookFn; // ATOMIK pointer yazimi
            VirtualProtect(slot, sizeof(void*), op, &op);
        }
    }
}

// Verilen swap chain'in Present (vt[8]) ve -- IDXGISwapChain1 ise -- Present1 (vt[22])
// vtable slotlarini atomik swap ile hook'la.
static void HookOneSwapchain(IDXGISwapChain* swap)
{
    void** vt = *reinterpret_cast<void***>(swap);
    DbgLog("HookOneSwapchain: vt[8]Present=%p", vt[8]);
    RecordVtablePatch(&vt[8], reinterpret_cast<void*>(&HookedPresent),
                 reinterpret_cast<void**>(&g_realPresent));

    IDXGISwapChain1* s1 = nullptr;
    if (SUCCEEDED(swap->QueryInterface(__uuidof(IDXGISwapChain1), reinterpret_cast<void**>(&s1))) && s1)
    {
        void** vt1 = *reinterpret_cast<void***>(s1);
        DbgLog("HookOneSwapchain: vt1[22]Present1=%p", vt1[22]);
        RecordVtablePatch(&vt1[22], reinterpret_cast<void*>(&HookedPresent1),
                     reinterpret_cast<void**>(&g_realPresent1));
        s1->Release();
    }
}

// Belirtilen swap-effect ile gecici bir swap chain olusturup Present adresini hook'lar.
static bool CreateHookDummy(DXGI_SWAP_EFFECT effect, UINT bufferCount, const wchar_t* cls)
{
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc   = DefWindowProcW;
    wc.hInstance     = g_selfModule;
    wc.lpszClassName = cls;
    RegisterClassExW(&wc);

    HWND dummy = CreateWindowExW(0, cls, L"", WS_OVERLAPPEDWINDOW,
                                 0, 0, 16, 16, nullptr, nullptr, g_selfModule, nullptr);
    if (!dummy) { UnregisterClassW(cls, g_selfModule); return false; }

    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount       = bufferCount;
    scd.BufferDesc.Width  = 16;
    scd.BufferDesc.Height = 16;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage       = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow      = dummy;
    scd.SampleDesc.Count  = 1;
    scd.Windowed          = TRUE;
    scd.SwapEffect        = effect;

    ID3D11Device*        dev  = nullptr;
    ID3D11DeviceContext* ctx  = nullptr;
    IDXGISwapChain*      swap = nullptr;

    // WARP (yazilim) surucusu: oyunun GERCEK GPU cihazina DOKUNMAZ, izole bir
    // yazilim rasterleyicidir -- tek amacimiz dxgi vtable adresini okumak oldugu
    // icin fazlasiyla yeterli ve en guvenlisi. Donanim cihazi + flip swapchain
    // olusturmak aktif render eden oyunlari cokertebiliyordu (olculen: PES 2021).
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &scd, &swap, &dev, nullptr, &ctx);

    DbgLog("CreateHookDummy: effect=%d bufferCount=%u hr=0x%08X swap=%p",
           (int)effect, bufferCount, (unsigned)hr, (void*)swap);

    bool ok = false;
    if (SUCCEEDED(hr) && swap)
    {
        HookOneSwapchain(swap);
        ok = true;
    }

    if (swap) swap->Release();
    if (ctx)  ctx->Release();
    if (dev)  dev->Release();
    DestroyWindow(dummy);
    UnregisterClassW(cls, g_selfModule);
    return ok;
}

static bool InstallHook()
{
    // TEK WARP dummy: FLIP_DISCARD modelinde. Neden sadece flip:
    //  - Modern oyunlar (PES dahil) flip-model kullanir; flip dummy'nin vt[8]
    //    Present ve vt[22] Present1 adresleri oyununkiyle AYNI paylasilan
    //    dxgi.dll fonksiyonlaridir (olculen: HookedPresent atesledi).
    //  - IDXGISwapChain tabanli vtable'in vt[8] slotu flip ve bitblt'te AYNIDIR
    //    (olculen: iki dummy ayni slotu verdi), yani flip dummy'nin Present yamasi
    //    eski bitblt oyunlarinin Present'ini de kapsar. Eski oyunlar Present1
    //    kullanmaz, dolayisiyla ayri bir bitblt dummy'ye gerek yok.
    //  - EN ONEMLISI: hook aktiflestikten ve oyun render thread'i artik bizim
    //    HookedPresent'imizden gectikten SONRA ikinci bir D3D11 cihazi olusturmak
    //    DXGI'nin surec-geneli kilidinde DEADLOCK yaratip oyunu donduruyordu
    //    (olculen: ikinci/bitblt dummy olusurken PES dondu). Tek cihaz bu riski
    //    ortadan kaldirir.
    //
    // WARP (yazilim) surucusu oyunun GPU cihazina dokunmaz; yalnizca dxgi.dll'deki
    // paylasilan Present/Present1 adresini okumak icin. RTSS/ReShade de ayni
    // paylasilan fonksiyonlari (vtable uzerinden) hook'lar.
    const int before = g_hookCount;

    // Flip-model (BufferCount>=2, MSAA yok, R8G8B8A8_UNORM -- CreateHookDummy'de ayarli).
    if (!CreateHookDummy(DXGI_SWAP_EFFECT_FLIP_DISCARD, 2, L"VLSS5LimiterDummyFlip"))
    {
        // Flip olusturulamadiysa (cok eski Windows / flip desteksiz) bitblt'e dus.
        // Bu durumda ikinci cihaz hook aktif DEGILKEN olusacagi icin deadlock riski yok.
        CreateHookDummy(DXGI_SWAP_EFFECT_DISCARD, 1, L"VLSS5LimiterDummyBitblt");
    }

    // Hicbir dummy olusamadiysa WARP basarisiz; durumu isaretle.
    if (g_hookCount == before && g_shared)
        g_shared->dllStatus = kStatusWarpFailed;

    // En az bir slot yamalandiysa DXGI hook'u kuruldu say.
    return g_hookCount > before;
}

// ---------------------------------------------------------------------------
// D3D9 vtable hook kurulumu -- yalnizca oyun d3d9.dll'i zaten yuklediyse.
//
// Gecici bir D3D9 cihazi olusturup IDirect3DDevice9 vtable'inin Present (index
// 17) girisini sarmalariz. vtable COM sinifi genelinde paylasildigi icin oyunun
// gercek cihazi da sarmalayiciyi cagirir. d3d9.dll yuklu degilse (oyun D3D9
// kullanmiyor) sessizce atlanir.
// ---------------------------------------------------------------------------
static bool InstallHookD3D9()
{
    HMODULE d3d9mod = GetModuleHandleW(L"d3d9.dll");
    if (!d3d9mod) return false; // oyun D3D9 kullanmiyor

    using Create9Fn = IDirect3D9* (WINAPI*)(UINT);
    auto pCreate9 = reinterpret_cast<Create9Fn>(GetProcAddress(d3d9mod, "Direct3DCreate9"));
    if (!pCreate9) return false;

    IDirect3D9* d3d = pCreate9(D3D_SDK_VERSION);
    if (!d3d) return false;

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc   = DefWindowProcW;
    wc.hInstance     = g_selfModule;
    wc.lpszClassName = L"VLSS5LimiterDummyWnd9";
    RegisterClassExW(&wc);
    HWND dummy = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW,
                                 0, 0, 16, 16, nullptr, nullptr, g_selfModule, nullptr);
    if (!dummy)
    {
        d3d->Release();
        UnregisterClassW(wc.lpszClassName, g_selfModule);
        return false;
    }

    // Windowed cihaz icin masaustu formatini kullan (D3DFMT_UNKNOWN bazi
    // surucu/sarmalayicilarda CreateDevice'i basarisiz kiliyordu).
    D3DDISPLAYMODE mode = {};
    D3DFORMAT bbFormat = D3DFMT_X8R8G8B8;
    if (SUCCEEDED(d3d->GetAdapterDisplayMode(D3DADAPTER_DEFAULT, &mode)) && mode.Format)
        bbFormat = mode.Format;

    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed         = TRUE;
    pp.SwapEffect       = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow    = dummy;
    pp.BackBufferFormat = bbFormat;
    // Backbuffer boyutunu ACIKCA ver: 16x16 pencerenin client alani ~0 olabilir
    // ve 0 backbuffer ile windowed CreateDevice BASARISIZ olur (D3D9 hook'unun
    // her oyunda kurulamamasinin sebebi buydu).
    pp.BackBufferWidth  = 32;
    pp.BackBufferHeight = 32;

    IDirect3DDevice9* dev = nullptr;
    HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, dummy,
                                   D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_NOWINDOWCHANGES,
                                   &pp, &dev);

    bool ok = false;
    if (SUCCEEDED(hr) && dev)
    {
        // IDirect3DDevice9 vtable: IUnknown 0-2, ... Present = index 17.
        // vt[17] slotunu atomik swap ile hook'la (DXGI ile ayni guvenli teknik).
        void** vtbl = *reinterpret_cast<void***>(dev);
        const int before = g_hookCount;
        RecordVtablePatch(&vtbl[17], reinterpret_cast<void*>(&HookedPresent9),
                       reinterpret_cast<void**>(&g_realPresent9));
        ok = (g_hookCount > before);
    }

    if (dev) dev->Release();
    d3d->Release();
    DestroyWindow(dummy);
    UnregisterClassW(wc.lpszClassName, g_selfModule);
    return ok;
}

// ---------------------------------------------------------------------------
// D3D8 vtable hook kurulumu -- yalnizca oyun d3d8.dll'i zaten yuklediyse.
//
// d3d8.h modern Windows SDK'da YOK. Bu yuzden IDirect3D8 / IDirect3DDevice8
// arayuzlerini acikca tanimlamak yerine, COM nesnesinin vtable'ini ham void**
// olarak alip gereken metotlari DOGRU INDEX ve imza ile cagiriyoruz. Enum/format
// degerleri D3D8 ve D3D9'da ayni oldugundan d3d9.h'dekiler kullanilir; yalnizca
// D3D8'in D3DPRESENT_PARAMETERS struct'i (layout D3D9'dan farkli) elle tanimli.
//
// Kullanilan vtable index'leri (standart, d3d8.h'den):
//   IUnknown::Release                 = 2
//   IDirect3D8::GetAdapterDisplayMode = 8
//   IDirect3D8::CreateDevice          = 15
//   IDirect3DDevice8::Present         = 15   <- sarmalanan
// ---------------------------------------------------------------------------
namespace
{
    // D3D8 D3DPRESENT_PARAMETERS (D3D9'dakinden FARKLI layout).
    struct D3DPP8
    {
        UINT                 BackBufferWidth;
        UINT                 BackBufferHeight;
        D3DFORMAT            BackBufferFormat;
        UINT                 BackBufferCount;
        D3DMULTISAMPLE_TYPE  MultiSampleType;
        D3DSWAPEFFECT        SwapEffect;
        HWND                 hDeviceWindow;
        BOOL                 Windowed;
        BOOL                 EnableAutoDepthStencil;
        D3DFORMAT            AutoDepthStencilFormat;
        DWORD                Flags;
        UINT                 FullScreen_RefreshRateInHz;
        UINT                 FullScreen_PresentationInterval;
    };
}

static bool InstallHookD3D8()
{
    HMODULE d3d8mod = GetModuleHandleW(L"d3d8.dll");
    if (!d3d8mod) return false; // oyun D3D8 kullanmiyor

    using Create8Fn = void* (WINAPI*)(UINT);
    auto pCreate8 = reinterpret_cast<Create8Fn>(GetProcAddress(d3d8mod, "Direct3DCreate8"));
    if (!pCreate8) return false;

    void* d3d8 = pCreate8(220); // D3D8 SDK surumu
    if (!d3d8) return false;
    void** d3d8vtbl = *reinterpret_cast<void***>(d3d8);

    using ReleaseFn   = ULONG   (STDMETHODCALLTYPE*)(void*);
    using GetModeFn   = HRESULT (STDMETHODCALLTYPE*)(void*, UINT, D3DDISPLAYMODE*);
    using CreateDevFn = HRESULT (STDMETHODCALLTYPE*)(void*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPP8*, void**);

    auto Release8 = reinterpret_cast<ReleaseFn>(d3d8vtbl[2]);

    // Windowed cihaz icin masaustu formatini al (D3D8 windowed, uyumlu format ister).
    D3DDISPLAYMODE mode = {};
    auto GetMode = reinterpret_cast<GetModeFn>(d3d8vtbl[8]);
    if (FAILED(GetMode(d3d8, 0 /*D3DADAPTER_DEFAULT*/, &mode)))
    {
        Release8(d3d8);
        return false;
    }

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc   = DefWindowProcW;
    wc.hInstance     = g_selfModule;
    wc.lpszClassName = L"VLSS5LimiterDummyWnd8";
    RegisterClassExW(&wc);
    HWND dummy = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW,
                                 0, 0, 16, 16, nullptr, nullptr, g_selfModule, nullptr);
    if (!dummy)
    {
        Release8(d3d8);
        UnregisterClassW(wc.lpszClassName, g_selfModule);
        return false;
    }

    D3DPP8 pp = {};
    pp.Windowed         = TRUE;
    pp.SwapEffect       = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow    = dummy;
    pp.BackBufferFormat = mode.Format;
    pp.BackBufferWidth  = 32;   // bkz. D3D9 notu: 0 backbuffer windowed'da basarisiz
    pp.BackBufferHeight = 32;

    void* dev = nullptr;
    auto CreateDev = reinterpret_cast<CreateDevFn>(d3d8vtbl[15]);
    HRESULT hr = CreateDev(d3d8, 0 /*D3DADAPTER_DEFAULT*/, D3DDEVTYPE_HAL, dummy,
                           D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev);

    bool ok = false;
    if (SUCCEEDED(hr) && dev)
    {
        // IDirect3DDevice8::Present = vtable index 15. Slotu atomik swap ile hook'la.
        void** devVtbl = *reinterpret_cast<void***>(dev);
        const int before = g_hookCount;
        RecordVtablePatch(&devVtbl[15], reinterpret_cast<void*>(&HookedPresent8),
                       reinterpret_cast<void**>(&g_realPresent8));
        ok = (g_hookCount > before);

        auto ReleaseDev = reinterpret_cast<ReleaseFn>(devVtbl[2]);
        ReleaseDev(dev);
    }

    Release8(d3d8);
    DestroyWindow(dummy);
    UnregisterClassW(wc.lpszClassName, g_selfModule);
    return ok;
}

static volatile LONG g_hooksRemoved = 0;

static void RemoveHook()
{
    InterlockedExchange(&g_shuttingDown, 1);

    // Birden fazla kez cagrilabilir (shutdown thread'i + DLL_PROCESS_DETACH);
    // temizligi yalnizca bir kez yap.
    if (InterlockedExchange(&g_hooksRemoved, 1) == 0)
    {
        // Tum vtable slotlarini (DXGI Present/Present1, D3D9, D3D8) atomik olarak
        // geri yukle: slot hala bizim sarmalayicilarimizdan birini gosteriyorsa
        // orijinali geri yaz. (Baska bir katman ustune yama yaptiysa dokunmayiz.)
        // Atomik pointer yazimi -- thread askiya alinmaz.
        for (int i = 0; i < g_vtablePatchCount; ++i)
        {
            void** slot = g_vtablePatches[i].slot;
            DWORD op = 0;
            if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &op))
            {
                void* cur = *slot;
                if (cur == reinterpret_cast<void*>(&HookedPresent)  ||
                    cur == reinterpret_cast<void*>(&HookedPresent1) ||
                    cur == reinterpret_cast<void*>(&HookedPresent9) ||
                    cur == reinterpret_cast<void*>(&HookedPresent8))
                    *slot = g_vtablePatches[i].orig;
                VirtualProtect(slot, sizeof(void*), op, &op);
            }
        }
    }

    if (g_shared)  { g_shared->dllAttached = 0; UnmapViewOfFile(g_shared); g_shared = nullptr; }
    if (g_mapFile) { CloseHandle(g_mapFile); g_mapFile = nullptr; }
}

// ---------------------------------------------------------------------------
// Kurulum thread'i -- DllMain icinde loader kilidi altinda is yapilmaz.
// ---------------------------------------------------------------------------
static DWORD WINAPI SetupThread(LPVOID)
{
    QueryPerformanceFrequency(&g_qpcFreq);

    if (!OpenSharedState())
    {
        // Ana uygulama kanali yok -> bu DLL'in yapacak isi yok. Hedefte asili
        // kalmak yerine kendini bosalt.
        FreeLibraryAndExitThread(g_selfModule, 0);
        return 0;
    }

    // Tani: oyunda hangi grafik API modulleri yuklu? (Hook'lanamayan bir oyunun
    // aslinda hangi API'yi kullandigini anlamak icin.)
    if (g_shared)
    {
        uint32_t mods = 0;
        if (GetModuleHandleW(L"d3d8.dll"))     mods |= kModD3D8;
        if (GetModuleHandleW(L"d3d9.dll"))     mods |= kModD3D9;
        if (GetModuleHandleW(L"dxgi.dll"))     mods |= kModDXGI;
        if (GetModuleHandleW(L"d3d11.dll"))    mods |= kModD3D11;
        if (GetModuleHandleW(L"d3d10.dll"))    mods |= kModD3D10;
        if (GetModuleHandleW(L"opengl32.dll")) mods |= kModOpenGL;
        if (GetModuleHandleW(L"ddraw.dll"))    mods |= kModDDraw;
        if (GetModuleHandleW(L"vulkan-1.dll")) mods |= kModVulkan;
        g_shared->loadedModules = mods;
    }

    DbgLog("SetupThread: basladi, paylasimli bellek acildi");

    // DXGI (D3D10/11/12), D3D9 ve D3D8 icin dummy cihazlari olusturup yamalanacak
    // vtable slotlarini KAYDET (henuz yazma). Oyun hangi API'yi kullaniyorsa onun
    // Present'i cagrilir; digerleri sarkitta kalir, zarari yok. D3D9/D3D8 yalnizca
    // ilgili dll yuklenmisse kurulur. Bu asamada HICBIR hook aktif DEGIL -- boylece
    // dummy cihaz olusturma/release islemleri hook'larla yarismaz (deadlock onlenir).
    const bool okDxgi = InstallHook();
    const bool okD3d9 = InstallHookD3D9();
    const bool okD3d8 = InstallHookD3D8();

    // Tum dummy cihazlar artik serbest; kaydedilen tum slotlari ATOMIK olarak yama.
    // Tek etkinlestirme noktasi burasi -- hook'lar bu satirdan sonra aktif.
    ApplyVtablePatches();

    DbgLog("Install bitti: okDxgi=%d okD3d9=%d okD3d8=%d hookCount=%d",
           okDxgi, okD3d9, okD3d8, g_hookCount);

    if (g_shared)
    {
        g_shared->installedHooks = (okDxgi ? kHookDXGI : 0) |
                                   (okD3d9 ? kHookD3D9 : 0) |
                                   (okD3d8 ? kHookD3D8 : 0);
        if (okDxgi || okD3d9 || okD3d8)
        {
            g_shared->dllStatus   = kStatusHookInstalled;
            g_shared->dllAttached = 1;
        }
        // Aksi halde dllStatus, InstallHook'un yazdigi hata kodunda (WARP/patch)
        // kalir; hicbiri kurulamadiysa hook yok demektir.
    }

    // Shutdown bekleme: ana uygulama shutdownSignal'i set edince (ya da kanal
    // kaybolunca) hook'u geri al ve DLL'i KENDINI BOSALT. Bu, "uninject" yolu --
    // onceki surumde DLL yalnizca oyun kapaninca ayriliyordu.
    while (!InterlockedCompareExchange(&g_shuttingDown, 0, 0))
    {
        if (!g_shared || g_shared->shutdownSignal != 0)
            break;
        Sleep(150);
    }

    RemoveHook();  // vtable geri yuklenir, paylasimli bellek birakilir
    Sleep(50);     // olasi "ucan" bir Present cagrisi orijinale donsun diye kisa pay
    FreeLibraryAndExitThread(g_selfModule, 0);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        g_selfModule = inst;
        DisableThreadLibraryCalls(inst);
        // Hook kurulumunu ayri thread'e at: D3D11CreateDeviceAndSwapChain
        // loader kilidi altinda cagrilamaz.
        CreateThread(nullptr, 0, SetupThread, nullptr, 0, nullptr);
        break;

    case DLL_PROCESS_DETACH:
        RemoveHook();
        break;
    }
    return TRUE;
}
