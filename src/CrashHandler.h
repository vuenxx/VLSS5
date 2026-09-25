#pragma once
#include "Common.h"

// Cokme (exception) ve donma (hang) tespiti icin tanisal dump uretimi.
// Uretilen .dmp dosyalari exe ile ayni klasore yazilir; Visual Studio veya
// WinDbg ile acilip cokme/donma anindaki tam call stack incelenebilir.
namespace CrashHandler
{
    // WinMain basinda bir kez cagrilir. Yakalanmamis bir exception (ornegin
    // GPU surucusunun attigi bir access violation) olustugunda otomatik olarak
    // vlss5_crash_<zaman>.dmp dosyasini yazar ve DLSS_Log'a kaydeder.
    void Install();

    // App'in Watchdog thread'i tarafindan cagrilir: render thread'i hicbir ilerleme
    // kaydetmeden uzun sure (SIKISMA_ESIGI_MS) ayni asamada takili kalirsa, o thread'i
    // gecici olarak durdurup ("suspend") o anki call stack'ini bir .dmp dosyasina yazar,
    // sonra devam ettirir ("resume"). Boylece program hala ekranda gorunmese/yanit
    // vermese bile, nerede kilitlendigini gosteren bir dosya elde edilir.
    void WriteHangDump(HANDLE hStuckThread, const char* stageName);

    // Render thread WriteHangDump'tan SONRA da uzun sure (KESIN_DONMA_ESIGI_MS)
    // hicbir ilerleme kaydetmezse cagrilir: bu artik "belki yavas bir islem"
    // degil, gercek bir deadlock/donma olarak kabul edilir. Son bir teshis dump'i
    // daha alir, kullaniciya kisa sureli (birkac saniye sonra kendiliginden kapanan)
    // bir bildirim gosterir ve sureci TerminateProcess ile sonlandirir.
    //
    // Amac: programin ekranda gorunmeden, %0 CPU ile arka planda sonsuza kadar
    // asili kalmasini engellemek. Kullanici artik Gorev Yoneticisi'nden elle
    // sonlandirmak zorunda kalmiyor; process kendini kapatiyor ve tek-ornek
    // mutex'i serbest birakiyor, boylece VLSS5 hemen yeniden baslatilabiliyor.
    void HandleConfirmedHang(HANDLE hStuckThread, const char* stageName);
}
