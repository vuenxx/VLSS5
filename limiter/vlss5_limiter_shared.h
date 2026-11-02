#pragma once
// ===========================================================================
// VLSS5 Dahili FPS Limiter -- Paylasimli Bellek Protokolu
//
// Ana uygulama (VLSS5.exe) ile hedef oyunun icinde calisan hook DLL'i
// (vlss5_limiter.dll) arasindaki TEK iletisim kanalidir. Named file-mapping
// uzerinden paylasilan kucuk bir struct; isim hedef surecin PID'si ile
// benzersiz kilinir, boylece ayni anda birden fazla oyun limitlense bile
// kanallar karismaz.
//
// TASARIM NOTU: Kanal TEK YONLU komut + TEK YONLU telemetridir. Ana uygulama
// yalnizca targetFps'i yazar; DLL yalnizca olculen degerleri (measuredFps,
// heartbeat) yazar. Karsilikli kilit yok -- her alan tek bir yazar tarafindan
// guncellenir, okuma tarafinda yirtik deger kabul edilebilir (bir sonraki
// karede duzelir). Bu yuzden struct'i buyutmek istersen alanlarin "tek yazar"
// kuralini koru.
// ===========================================================================

#include <cstdint>

namespace vlss5::limiter
{
    // Paylasilan struct'in ABI surumu. Ana uygulama ve DLL ayri derlendigi icin
    // (ayri .vcxproj) uyumsuz bir surum yuklenirse DLL kendini devre disi birakir.
    // Protokolu her degistirdiginde bunu ARTIR.
    static constexpr uint32_t kProtocolVersion = 3;

    // installedHooks / loadedModules bit maskeleri (tani). Hangi hook'un kuruldugunu
    // ve oyunun HANGI grafik API modullerini yukledigini gosterir -- "hook kuruldu
    // ama FPS sabitlenmiyor" durumunu teshis etmek icin (oyun baska/daha eski bir
    // API kullaniyor olabilir).
    enum HookBit   : uint32_t { kHookDXGI = 1, kHookD3D9 = 2, kHookD3D8 = 4 };
    enum ModuleBit : uint32_t {
        kModD3D8 = 1, kModD3D9 = 2, kModDXGI = 4, kModD3D11 = 8,
        kModD3D10 = 16, kModOpenGL = 32, kModDDraw = 64, kModVulkan = 128,
    };

    // DLL kurulum asamasi / hata kodu (dllStatus). Ana uygulama tani icin okur.
    // Mapping hic acilamazsa (isim yok / protokol uyumsuz) DLL hicbir sey yazamaz
    // ve deger kStatusNone kalir -- bu durumda "DLL yuklendi mi" sorusu uzak
    // thread'in cikis koduyla ayrilir.
    enum DllStatus : int32_t
    {
        kStatusNone          = 0, // DLL yazmadi (yuklenmedi veya mapping/protokol acilamadi)
        kStatusSharedOpened  = 2, // paylasimli bellek acildi, protokol uyumlu
        kStatusHookInstalled = 3, // Present hook kuruldu (dllAttached=1 ile)
        kStatusWarpFailed    = 4, // vtable icin D3D11 WARP cihazi/zinciri olusturulamadi
        kStatusPatchFailed   = 5, // VirtualProtect / vtable yamasi basarisiz
    };

    // File-mapping ve olaylarin isim onekleri. Tam isim: <onek><pid>.
    // "Local\\" oneki: ayni oturumdaki sureclerle sinirli (oyun ve VLSS5 ayni
    // kullanici oturumunda kostugu icin yeterli ve en dar kapsam).
    static constexpr wchar_t kMappingPrefix[]   = L"Local\\VLSS5_Limiter_Map_";
    static constexpr wchar_t kConfigEventPrefix[] = L"Local\\VLSS5_Limiter_Cfg_";

    // targetFps icin ozel degerler.
    static constexpr int32_t kFpsUnlimited = 0;   // sinir yok (limiter pasif bekler)
    static constexpr int32_t kFpsDisabled  = -1;  // DLL kendini tamamen kapatsin

    // Bekleme stratejisi. Saf sleep CPU'yu bosaltir ama ~1-2 ms jitter birakir;
    // saf spin tam isabetli ama bir cekirdegi doldurur. Hibrit: hedefin biraz
    // altina kadar uyu, kalan kucuk payi busy-wait ile kapat.
    enum class WaitMode : int32_t
    {
        Hybrid = 0,   // varsayilan: uyu + son payda spin
        Sleep  = 1,   // yalnizca uyku (dusuk CPU, daha gevsek)
        Spin   = 2,   // yalnizca spin (en isabetli, yuksek CPU)
    };

#pragma pack(push, 8)
    struct SharedState
    {
        // ---- Ana uygulama tarafindan yazilir, DLL okur ----
        uint32_t protocolVersion;   // = kProtocolVersion; uyusmazsa DLL cekilir
        int32_t  targetFps;         // istenen FPS; 0 = sinirsiz, -1 = kapat
        int32_t  waitMode;          // WaitMode

        // ---- DLL tarafindan yazilir, ana uygulama okur ----
        uint32_t dllAttached;       // DLL hook'u basariyla kurduysa 1
        uint32_t presentApi;        // 0=bilinmiyor, 11=D3D11/DXGI, 12=D3D12
        int32_t  measuredFps;       // DLL'in Present frekansindan olctugu gercek FPS
        int32_t  dllStatus;         // DllStatus: kurulum asamasi / hata kodu
        uint32_t shutdownSignal;    // ana uygulama 1 yazar -> DLL hook'u geri alip
                                    // KENDINI BOSALTIR (uninject). Boylece oyun
                                    // kapanmadan da temizce ayrilir.
        uint64_t heartbeat;         // DLL her ~0.5 sn'de artirir; canlilik gostergesi
        uint64_t presentCount;      // toplam sarmalanan Present cagrisi (tani)
        uint32_t installedHooks;    // HookBit maskesi: hangi hook(lar) kuruldu
        uint32_t loadedModules;     // ModuleBit maskesi: oyunda yuklu grafik API'leri
    };
#pragma pack(pop)

    // Her iki tarafin da ayni struct boyutunu gordugunu derleme aninda dogrula.
    // 10 x uint32 (40) + 2 x uint64 (16) = 56 bayt.
    static_assert(sizeof(SharedState) == 56, "SharedState ABI degisti -- kProtocolVersion'i artir");
}
