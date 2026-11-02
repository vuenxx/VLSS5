#pragma once
#include "Common.h"
#include "../limiter/vlss5_limiter_shared.h"

// ===========================================================================
// LimiterManager
//
// Kalibrasyonun (App::UpdateCalibration) FPS limitini NASIL uyguladigindan
// soyutlanmasini saglar. Eskiden kalibrasyon dogrudan RTSSManager'a cagri
// yapiyordu; artik secili ARKA UCA gore yonlendirilir:
//
//   Backend::Rtss      -> RTSSManager (profil .cfg'ye yaz, enjeksiyon YOK)
//   Backend::Injection -> vlss5_limiter.dll'i hedefe yukle, FPS'i paylasimli
//                         bellekten canli ayarla (opt-in, varsayilan KAPALI)
//
// Enjeksiyon arka uci yalnizca kullanici ayarlardan acikca actiginda ve
// ban uyarisini onayladiginda kullanilir. Varsayilan daima Rtss'tir.
//
// Sozlesme (kalibrasyonun bekledigi):
//   Begin(pid, exe)    -> hedefi kilitler, onceki durumu saklar
//   ApplyLimit(fps)    -> 0 = sinirsiz; canli uygulanir
//   MeasuredFps()      -> arka uc olcebiliyorsa gercek FPS, yoksa -1
//   Restore()          -> onceki durumu geri yukler ve kaynaklari birakir
// ===========================================================================

class LimiterManager
{
public:
    enum class Backend { Rtss, Injection };

    static LimiterManager& Get();

    // Kullanicinin ayarladigi arka uc. Enjeksiyon yalnizca
    // allowInjection (ayar + onay) true ise gecerli sayilir.
    void SetBackend(Backend b) { m_backend = b; }
    Backend GetBackend() const { return m_backend; }

    // Bir kalibrasyon/limit oturumu baslat. Secili arka uca gore RTSS profilini
    // hazirlar ya da DLL'i hedefe yukler. Basarisizsa false (cagiran RTSS'e
    // dusebilir). exeName RTSS icin, pid enjeksiyon icin gerekli.
    bool Begin(DWORD pid, const std::wstring& exeName);

    // FPS limitini uygula. 0 = sinirsiz. Canli; 3 sn'de bir cagrilabilir.
    bool ApplyLimit(int fps);

    // Arka uc gercek FPS olcebiliyorsa dondurur (enjeksiyon: DLL telemetrisi);
    // olcemiyorsa -1 (RTSS: GetLiveFps ayrica kullanilir).
    int MeasuredFps() const;

    // Enjeksiyon arka ucunun DLL'i gercekten hedefe baglandi mi? RTSS'te daima
    // true (ayri surec). Kalibrasyon bunu "arka uc hazir mi" diye sorabilir.
    bool IsBackendLive() const;

    // Enjeksiyon DLL'inin yazdigi kurulum asamasi / hata kodu (vlss5::limiter::
    // DllStatus). Hook kurulamadiysa NEDENINI gosterir; tani/log icin. RTSS'te 0.
    int DllStatus() const;

    // Tani maskeleri (enjeksiyon): hangi hook'lar kuruldu (HookBit) ve oyunda
    // hangi grafik API modulleri yuklu (ModuleBit). "Hook var ama FPS sabitlenmiyor"
    // durumunda oyunun gercekte hangi API'yi kullandigini gosterir.
    int InstalledHooks() const;
    int LoadedModules() const;
    // Hangi Present gercekten cagrildi (DLL calisma aninda yazar): 0=hicbiri,
    // 8=D3D8, 9=D3D9, 11=DXGI. 0 ise oyunun gercek present'i hook'ladigimiz
    // hicbir API'den gecmiyor demektir.
    int PresentApi() const;

    // Limit oturumunu bitir ama enjeksiyonu KORU: RTSS'te onceki limiti geri
    // yukler; enjeksiyonda yalnizca targetFps'i sinirsiza ceker, DLL yuklu kalir.
    // Boylece ayni overlay oturumunda tekrar kalibrasyon YENIDEN ENJEKTE ETMEDEN
    // limit degistirebilir (ikinci LoadLibrary DllMain'i tekrar calistirmaz --
    // reuse sart). Iptal/abort ve iki kalibrasyon arasi bunu kullanir.
    void Restore();

    // Enjeksiyon oturumunu TAMAMEN kapat: DLL'e shutdownSignal yazilir (kendini
    // boşaltir/uninject), kaynaklar birakilir. RTSS'te Restore ile ayni. Overlay
    // tamamen dururken (App::StopOverlay) cagrilir.
    void Shutdown();

    // Son hata metni (UI/log icin).
    const std::wstring& LastError() const { return m_lastError; }

private:
    LimiterManager() = default;

    // ---- Enjeksiyon arka uci ----
    bool InjectionBegin(DWORD pid);
    void InjectionRestore();
    std::wstring LimiterDllPath() const;    // exe yanindaki vlss5_limiter.dll (64-bit)
    std::wstring LimiterDll32Path() const;  // exe yanindaki vlss5_limiter32.dll (32-bit)
    std::wstring InjectorExe32Path() const; // exe yanindaki vlss5_inject32.exe (yardimci)

    // 32-bit (WOW64) hedefe, 32-bit yardimci exe uzerinden vlss5_limiter32.dll'i
    // enjekte eder. 64-bit uygulamadan 32-bit surece dogrudan enjeksiyon capraz
    // mimari sorunlu oldugu icin is kendi mimarisinde kosan yardimciya devredilir.
    bool LaunchInjector32(DWORD pid, const std::wstring& dll32Path);

    Backend      m_backend     = Backend::Rtss;
    std::wstring m_lastError;

    // Limit oturumu durumu (Begin/Restore ile).
    bool         m_active       = false;
    DWORD        m_pid          = 0;
    std::wstring m_exeName;

    // Enjeksiyon oturumu durumu (overlay boyunca yasar; Shutdown ile biter).
    // DLL su an hedefe yuklu mu ve hangi PID'e? Reuse karari bununla verilir.
    bool         m_injected     = false;
    DWORD        m_injectedPid  = 0;

    // RTSS: kullanicinin oturum oncesi limiti (geri yukleme icin).
    int          m_rtssOriginalLimit = 0;

    // Enjeksiyon: paylasimli bellek + hedef surec tutamaci.
    HANDLE                         m_mapFile = nullptr;
    vlss5::limiter::SharedState*   m_shared  = nullptr;
    HANDLE                         m_process = nullptr;
};
