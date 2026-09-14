#pragma once
#include "Common.h"

struct Dlss5Config
{
    int   preset         = 0;       // 0=Default, 1=Preset 1, 2=Preset 2, 3=Preset 3
    int   style          = 0;       // 0=Standard (Keskin), 1=Natural (Doğal), 2=Cinematic (Film)
    float intensity      = 1.0f;    // 0.0 - 2.0
    float localStructure = 1.0f;    // 0.0 - 2.0 (Yüzey detay ve geometri keskinliği)
    float localTone      = 1.0f;    // 0.0 - 2.0 (Mikro kontrast)
    float skinStructure  = -1.0f;   // -1.0 = Otomatik / takip et, 0.0 - 2.0 = Manuel ten ayarı
    bool  useAutoMask    = true;    // Otomatik ten maskesi
    int   resolutionScale = 100;    // 50 - 100 (% Model Çözünürlüğü / Performans Modu)
    bool  temporalStabilizer = false;// Gölge & hareket sabitleyici (Zorunlu reset bayrağı)
    bool  opticalFlow        = true; // Optik akış hareket vektörleri (GPU tabanlı gerçek zamanlı hareket takibi)
    float boostFactor        = 1.0f; // 1.0 - 2.5 (Nöral Etki Yoğunluğu / Extrapolation Boost)
    bool  splitScreen        = false;// Bölünmüş Ekran (Karşılaştırma Modu)
<<<<<<< Updated upstream
=======

    // Direct Flip (Deneysel): overlay penceresinden WS_EX_LAYERED kaldırılır.
    // AÇIK  -> Frame Generation altında Present() stall'ı kalkar (24ms -> ~0ms),
    //          ancak pencere OS hit-test zincirine girer; fare geçirgenliği
    //          yalnızca HTTRANSPARENT'a bağlı kalır ve bazı oyunlarda bozulur.
    // KAPALI -> Klasik layered overlay. Fare/imleç davranışı her oyunda doğru.
    // Varsayılan KAPALI: doğruluk, deneysel hızdan önce gelir.
    bool  directFlip         = false;

    // Tam Ekran Yap: overlay hedef pencerenin degil, hedefin bulundugu MONITORUN
    // tamamini kaplar; yakalanan kare cikis cozunurlugune gerilir.
    // DLSS/MV yigini yakalama cozunurlugunde calismaya devam eder; gerdirme yalnizca
    // son gecerde (full-screen ucgen, lineer filtre) uygulanir.
    bool  fullscreenStretch  = false;
>>>>>>> Stashed changes
    float splitPos           = 0.5f; // 0.0 - 1.0 (Bölünme Çizgisi Konumu, varsayılan %50)

    // Settings window toggle hotkey (Default: INSERT)
    UINT  settingsVk     = VK_INSERT;
    UINT  settingsMod    = 0;       // MOD_CONTROL, MOD_ALT, MOD_SHIFT vs. (0 = bare key)

    // Selected GPU adapter description (e.g. L"Auto" or specific name)
    std::wstring selectedGpu = L"Auto";
    
    // RTSS directory path
    std::wstring rtssDirectory = L"";

    std::wstring FormatHotkey() const;
};

class ConfigManager
{
public:
    static ConfigManager& Get();

    void Load();
    void Save();

    Dlss5Config& Config() { return m_config; }
    const Dlss5Config& Config() const { return m_config; }

private:
    ConfigManager() { Load(); }
    std::wstring GetIniPath() const;

    Dlss5Config m_config;
};
