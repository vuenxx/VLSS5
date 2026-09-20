#pragma once
#include "Common.h"

struct Dlss5Config
{
    int   preset         = 0;       // 0=Default, 1=Preset 1, 2=Preset 2, 3=Preset 3
    int   style          = 0;       // 0=Standard (Keskin), 1=Natural (Doğal), 2=Cinematic (Film)
    float intensity      = 1.0f;    // 0.0 - 1.0 (shader saturate() uyguluyor, ustu anlamsiz)
    float localStructure = 1.0f;    // 0.0 - 2.0 (Yüzey detay ve geometri keskinliği)
    float localTone      = 1.0f;    // 0.0 - 2.0 (Mikro kontrast)
    float skinStructure  = -1.0f;   // -1.0 = Otomatik / takip et, 0.0 - 2.0 = Manuel ten ayarı
    bool  useAutoMask    = true;    // Otomatik ten maskesi
    int   resolutionScale = 100;    // 50 - 100 (% Model Çözünürlüğü / Performans Modu)

    // Çok Geçişli (Multipass) Nöral İşleme.
    // Model, kendi çıktısını tekrar girdi olarak alıp N kez çalışır. Her geçişin
    // KENDİ feature handle'i (ve dolayısıyla kendi temporal geçmişi) vardır --
    // tek handle'i tekrar kullanmak geçmişi bozar ve sadece renk kaymasına yol
    // açar (OptiScaler'daki davranış). Maliyet geçiş sayısıyla doğrusal artar;
    // ara geçişler paylaşılmayan native D3D12 dokularında yapılır ve TEK komut
    // listesinde gönderilir, böylece D3D11<->D3D12 senkron bedeli bir kez ödenir.
    int   passCount      = 1;       // 1 - 4 (1 = multipass kapalı)
    float passFalloff    = 1.0f;    // 0.25 - 1.0 (2..N. geçişlerde intensity çarpanı)
    bool  temporalStabilizer = false;// Gölge & hareket sabitleyici (Zorunlu reset bayrağı)
    bool  opticalFlow        = true; // Optik akış hareket vektörleri (GPU tabanlı gerçek zamanlı hareket takibi)
    float boostFactor        = 1.0f; // 1.0 - 2.5 (Nöral Etki Yoğunluğu / Extrapolation Boost)
    bool  splitScreen        = false;// Bölünmüş Ekran (Karşılaştırma Modu)

    // Overlay kompozisyon modu -- ÖLÇÜLEN darboğaz buradan geçiyor.
    //
    // Boru hattının tamamı (yakalama + downscale + optik akış + DLSS-NR + kompozit)
    // ~1.7 ms sürüyor; Present() ise 40-95 ms, patolojik durumda 1000 ms. Yani
    // maliyetin tamamı DWM kompozisyonunda. Sebep adayı: overlay WS_EX_LAYERED +
    // LWA_ALPHA 254 ile YARI SAYDAM bir pencere. DWM her kompozisyonda tam ekran
    // per-pixel alpha karışımı yapmak zorunda ve DirectFlip yolu kapanıyor.
    //
    // Alpha 254 kasıtlıydı: tam opak bir overlay altındaki oyunu DWM "tamamen
    // kapatıldı" sayıp throttle edebilir, o zaman da yakalayacak kare kalmaz.
    // Hangi tarafın daha ucuz olduğu donanıma/oyuna göre değişir, o yüzden mod.
    //
    // 0 = Uyumlu   : LAYERED + alpha 254 (mevcut davranış; oyun throttle olmaz)
    // 1 = Opak     : LAYERED + alpha 255 (DWM opak muamelesi yapabilir)
    // 2 = En Hızlı : LAYERED yok, tamamen opak (DirectFlip yolu açılabilir)
    // Overlay kompozisyon sinifi (App::ApplyOverlayComposition):
    //   0 = Uyumlu (layered, alpha 254)  1 = Opak (layered, alpha 255)  2 = En hizli (layered yok)
    // Artik arayuzden degistirilmiyor; olculen en iyi denge 1'de kaldi.
    // Deneyecekler icin INI'deki [VLSS5] OverlayMode anahtari hala okunuyor.
    int   overlayMode        = 1;



    // Tam Ekran Yap: overlay hedef pencerenin degil, hedefin bulundugu MONITORUN
    // tamamini kaplar; yakalanan kare cikis cozunurlugune gerilir.
    // DLSS/MV yigini yakalama cozunurlugunde calismaya devam eder; gerdirme yalnizca
    // son gecerde (full-screen ucgen, lineer filtre) uygulanir.
    bool  fullscreenStretch  = false;

    float splitPos           = 0.5f; // 0.0 - 1.0 (Bölünme Çizgisi Konumu, varsayılan %50)

    // Settings window toggle hotkey (Default: INSERT)
    UINT  settingsVk     = VK_INSERT;
    UINT  settingsMod    = 0;       // MOD_CONTROL, MOD_ALT, MOD_SHIFT vs. (0 = bare key)

    // New configurable hotkeys
    UINT  vkFgIndicator = VK_F7;
    UINT  vkFocus       = VK_F8;
    UINT  vkFps         = VK_F9;
    UINT  vkToggleVlss  = VK_F10;
    UINT  vkCalib       = VK_F2;
    UINT  vkStart       = 'S';
    UINT  modStart      = MOD_ALT; // Modifiers for Start toggle

    // Yakalama + sunum (D3D11 cihazi, WGC, swap chain) icin GPU aciklamasi.
    // Monitoru hangi kart suruyorsa o secilmelidir; flip-model sunum ekrani
    // suren adapterde olmak zorundadir.
    // L"Auto" = RTX oncelikli otomatik secim.
    std::wstring selectedGpu = L"Auto";

    // DLSS / D3D12 sinir aglarinin kosacagi GPU aciklamasi.
    // selectedGpu ile ayni cikarsa tek-adapter hizli yol kullanilir; farkli
    // cikarsa D3D12Interop cross-adapter kopru moduna gecer (PCIe uzerinden
    // kare basina giris + hareket vektoru + cikis transferi).
    // L"Auto" = RTX oncelikli otomatik secim.
    std::wstring dlssGpu = L"Auto";
    
    // RTSS directory path
    std::wstring rtssDirectory = L"";

    std::wstring FormatHotkey() const;
    static std::wstring FormatKey(UINT vk, UINT mod = 0);
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

    // INI dosyasi profil API'sinin dogru okuyamayacagi bir hale gelmis mi?
    // (UTF-8 BOM veya mukerrer [VLSS5] bolumu -- ayrinti icin .cpp'deki nota bak)
    bool IniNeedsRepair(const std::wstring& path) const;

    // Dosyayi silip mevcut (yani API'nin gercekten okudugu) degerlerle tek
    // bolumlu, UTF-16LE temiz bir dosya olarak yeniden yazar.
    void RepairIni(const std::wstring& path);

    Dlss5Config m_config;
};
