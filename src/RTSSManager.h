#pragma once
#include <string>
#include <vector>

class RTSSManager
{
public:
    static RTSSManager& Get();

    // Returns a list of all .cfg files in the RTSS Profiles directory
    std::vector<std::wstring> GetProfiles();

    // Sets the framerate limit for a specific executable (e.g. "Game.exe").
    // limit = 0  -> RTSS'te SINIRSIZ demektir.
    // limit < 0  -> gecersiz; yazilmaz, false doner. (Kalibrasyon bir zamanlar
    //               sinirsiz asagi sayip negatif deger yaziyordu.)
    bool SetFramerateLimit(const std::wstring& exeName, int limit);

    // Profildeki mevcut limiti okur. Profil/anahtar yoksa 0 (sinirsiz) doner.
    // Kalibrasyon oncesi kullanicinin ayarini saklayip iptal/hata halinde geri
    // yukleyebilmek icin gerekli.
    int  GetFramerateLimit(const std::wstring& exeName);

    // Forces RTSS to reload
    void NotifyRTSS();

    std::wstring GetProfilesDir() const;

    // RTSS'in "RTSSSharedMemoryV2" paylasimli bellegini okuyup, verilen process'in
    // RTSS'in OYUNUN ICINE hook'layarak dogrudan olctugu, EKRAN YAKALAMADAN (WGC/DXGI'dan)
    // tamamen bagimsiz gercek kare hizini dondurur. Bunun gerekli olma sebebi: bizim
    // topmost/opak overlay'imiz hedefin TAM USTUNDE oldugundan, DWM composition tabanli
    // her olcum (WGC FrameArrived, DXGI Duplication) kendi render hizimizla kirleniyor --
    // RTSS ise Present() cagrisini oyunun D3D katmaninda, compositor'dan ONCE yakaladigi
    // icin bu kirlenmeden tamamen bagimsiz. Basarisiz olursa (RTSS kapali/hook yok/pid
    // bulunamadi) -1 doner; cagiran taraf bu durumda kendi (yaklasik) olcumune dusmeli.
    int GetLiveFps(unsigned long processId);

private:
    RTSSManager() = default;

    bool EnsureSharedMemoryMapped();

    void* m_rtssMapping = nullptr; // HANDLE (file mapping)
    void* m_rtssView    = nullptr; // mapped LPRTSS_SHARED_MEMORY
};
