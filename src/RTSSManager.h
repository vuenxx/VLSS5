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

private:
    RTSSManager() = default;
};
