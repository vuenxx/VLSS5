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

    // Settings window toggle hotkey (Default: INSERT)
    UINT  settingsVk     = VK_INSERT;
    UINT  settingsMod    = 0;       // MOD_CONTROL, MOD_ALT, MOD_SHIFT vs. (0 = bare key)

    // Selected GPU adapter description (e.g. L"Auto" or specific name)
    std::wstring selectedGpu = L"Auto";

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
