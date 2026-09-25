#pragma once
#include "Common.h"
#include "ConfigManager.h"

// ---------------------------------------------------------------------------
// Bir on ayarin (preset) hedef kimligi.
//   - Process: normal pencere yakalamasi, TAM exe yoluyla eslestirilir.
//   - Monitor: "Tum Ekran" yakalamasi, kalici monitor cihaz adiyla eslestirilir.
// Eslesme HER ZAMAN tam/birebir yapilir -- fallback yoktur.
// ---------------------------------------------------------------------------
struct PresetIdentity
{
    bool         isMonitor = false;
    std::wstring exeFullPath;   // isMonitor == false iken gecerli
    std::wstring monitorDevice; // isMonitor == true iken gecerli (orn. L"\\.\DISPLAY1")

    // Sadece GORUNUM icin -- eslesmede (IdentityEquals) KULLANILMAZ. Process
    // hedeflerinde pencere basligini da gosterebilmek icin (orn. "Half-Life (hl.exe)").
    std::wstring windowTitle;
};

struct PresetEntry
{
    std::wstring   folderName;  // diskte, kozmetik ama benzersiz
    std::wstring   displayName; // UI'da gosterilir (meta.ini'den)
    PresetIdentity identity;    // meta.ini'den okunur -- klasor adindan ASLA
};

// ---------------------------------------------------------------------------
// PresetsManager
//   VLSS5.exe'nin yanindaki "presets\" klasorunde, pencere/oyun (veya tam ekran
//   monitor hedefi) basina bir alt klasor olarak nöral ayar on ayarlarini saklar.
//   RTSSManager ile ayni desen: GetPrivateProfileString/IntW + WritePrivateProfileStringW.
// ---------------------------------------------------------------------------
class PresetsManager
{
public:
    static PresetsManager& Get();

    // exe-yani "presets\" klasoru; yoksa olusturulur.
    std::wstring GetPresetsRootDir() const;

    std::vector<PresetEntry> ListPresets() const;

    // Verilen kimlige TAM eslesen preset'i dondurur; yoksa folderName bos doner.
    PresetEntry FindPresetForIdentity(const PresetIdentity& id) const;

    bool LoadPresetConfig(const std::wstring& folderName, Dlss5Config& outCfg) const;

    // 'id' icin zaten bir preset varsa sessizce uzerine yazar; yoksa yeni (deduped)
    // bir klasor acar. Yazilan klasor adini dondurur, hata halinde "".
    std::wstring SavePreset(const PresetIdentity& id, const Dlss5Config& cfg);

    bool DeletePreset(const std::wstring& folderName);

private:
    PresetsManager() = default;

    std::wstring MakeCosmeticFolderName(const PresetIdentity& id) const;
    std::wstring DedupeFolderName(const std::wstring& base, const PresetIdentity& id) const;
    bool ReadMeta(const std::wstring& folderPath, PresetIdentity& outId, std::wstring& outDisplayName) const;
    void WriteMeta(const std::wstring& folderPath, const PresetIdentity& id, const std::wstring& displayName) const;

    // Dosya yoksa UTF-16LE BOM ile olusturur (WritePrivateProfileStringW Unicode
    // yazabilsin diye -- bkz. ConfigManager::RepairIni'deki ayni gerekce).
    void EnsureBom(const std::wstring& filePath) const;

    static bool IdentityEquals(const PresetIdentity& a, const PresetIdentity& b);
};
