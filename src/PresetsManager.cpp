#include "PresetsManager.h"
#include <shlwapi.h>
#include <cstdio>

#pragma comment(lib, "shlwapi.lib")

static std::wstring SanitizeForFolderName(std::wstring s)
{
    for (wchar_t& c : s)
    {
        if (c == L'\\' || c == L'/' || c == L':' || c == L'*' || c == L'?' ||
            c == L'"'  || c == L'<' || c == L'>' || c == L'|')
            c = L'_';
    }
    while (!s.empty() && (s.back() == L'.' || s.back() == L' '))
        s.pop_back();
    if (s.size() > 80) s.resize(80);
    if (s.empty()) s = L"Preset";
    return s;
}

static std::wstring MonitorShortName(const std::wstring& device)
{
    std::wstring dev = device;
    size_t pos = dev.find_last_of(L'\\');
    if (pos != std::wstring::npos) dev = dev.substr(pos + 1);
    return dev;
}

static std::wstring MakeDisplayName(const PresetIdentity& id)
{
    if (id.isMonitor)
    {
        return L"Tüm Ekran (" + MonitorShortName(id.monitorDevice) + L")";
    }

    wchar_t exeCopy[MAX_PATH] = {};
    wcsncpy_s(exeCopy, id.exeFullPath.c_str(), _TRUNCATE);
    std::wstring exeName = PathFindFileNameW(exeCopy);

    // Pencere basligi varsa "Pencere Adi (exe.exe)" seklinde goster -- sadece
    // exe adi kullaniciya hangi oyun oldugunu soylemiyordu (bkz. kullanici geri
    // bildirimi: "Half-Life_hl.exe" gibi kod adlari anlamsiz kaliyordu).
    if (!id.windowTitle.empty())
        return id.windowTitle + L" (" + exeName + L")";

    return exeName;
}

PresetsManager& PresetsManager::Get()
{
    static PresetsManager instance;
    return instance;
}

std::wstring PresetsManager::GetPresetsRootDir() const
{
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    PathRemoveFileSpecW(path);
    PathCombineW(path, path, L"presets");

    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES)
        CreateDirectoryW(path, nullptr);

    return path;
}

bool PresetsManager::IdentityEquals(const PresetIdentity& a, const PresetIdentity& b)
{
    if (a.isMonitor != b.isMonitor) return false;
    if (a.isMonitor) return _wcsicmp(a.monitorDevice.c_str(), b.monitorDevice.c_str()) == 0;
    return _wcsicmp(a.exeFullPath.c_str(), b.exeFullPath.c_str()) == 0;
}

void PresetsManager::EnsureBom(const std::wstring& filePath) const
{
    if (GetFileAttributesW(filePath.c_str()) != INVALID_FILE_ATTRIBUTES) return;

    // WritePrivateProfileStringW BOM'suz yeni dosyayi ANSI kabul eder; Turkce
    // karakterler (exe yolu, gorunen ad) bu durumda bozulur -- bkz.
    // ConfigManager::RepairIni'deki ayni gerekce.
    HANDLE h = CreateFileW(filePath.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE)
    {
        const unsigned char bom[2] = { 0xFF, 0xFE };
        DWORD written = 0;
        WriteFile(h, bom, sizeof(bom), &written, nullptr);
        CloseHandle(h);
    }
}

bool PresetsManager::ReadMeta(const std::wstring& folderPath, PresetIdentity& outId, std::wstring& outDisplayName) const
{
    std::wstring metaPath = folderPath + L"\\meta.ini";
    if (GetFileAttributesW(metaPath.c_str()) == INVALID_FILE_ATTRIBUTES) return false;

    wchar_t kindBuf[16] = {};
    GetPrivateProfileStringW(L"Meta", L"Kind", L"", kindBuf, _countof(kindBuf), metaPath.c_str());
    outId.isMonitor = (_wcsicmp(kindBuf, L"Monitor") == 0);

    wchar_t buf[MAX_PATH] = {};
    if (outId.isMonitor)
    {
        GetPrivateProfileStringW(L"Meta", L"MonitorDevice", L"", buf, _countof(buf), metaPath.c_str());
        outId.monitorDevice = buf;
    }
    else
    {
        GetPrivateProfileStringW(L"Meta", L"ExePath", L"", buf, _countof(buf), metaPath.c_str());
        outId.exeFullPath = buf;
    }

    wchar_t nameBuf[256] = {};
    GetPrivateProfileStringW(L"Meta", L"DisplayName", L"", nameBuf, _countof(nameBuf), metaPath.c_str());
    outDisplayName = nameBuf;

    return true;
}

void PresetsManager::WriteMeta(const std::wstring& folderPath, const PresetIdentity& id, const std::wstring& displayName) const
{
    std::wstring metaPath = folderPath + L"\\meta.ini";
    EnsureBom(metaPath);

    WritePrivateProfileStringW(L"Meta", L"Kind", id.isMonitor ? L"Monitor" : L"Process", metaPath.c_str());
    WritePrivateProfileStringW(L"Meta", L"ExePath", id.isMonitor ? L"" : id.exeFullPath.c_str(), metaPath.c_str());
    WritePrivateProfileStringW(L"Meta", L"MonitorDevice", id.isMonitor ? id.monitorDevice.c_str() : L"", metaPath.c_str());
    WritePrivateProfileStringW(L"Meta", L"DisplayName", displayName.c_str(), metaPath.c_str());
}

std::wstring PresetsManager::MakeCosmeticFolderName(const PresetIdentity& id) const
{
    if (id.isMonitor)
    {
        return SanitizeForFolderName(L"Monitor_" + MonitorShortName(id.monitorDevice));
    }

    wchar_t exeCopy[MAX_PATH] = {};
    wcsncpy_s(exeCopy, id.exeFullPath.c_str(), _TRUNCATE);
    std::wstring exeName = PathFindFileNameW(exeCopy);

    wchar_t dirCopy[MAX_PATH] = {};
    wcsncpy_s(dirCopy, id.exeFullPath.c_str(), _TRUNCATE);
    PathRemoveFileSpecW(dirCopy);
    std::wstring parentDir = PathFindFileNameW(dirCopy);

    std::wstring combined = parentDir.empty() ? exeName : (parentDir + L"_" + exeName);
    return SanitizeForFolderName(combined);
}

std::wstring PresetsManager::DedupeFolderName(const std::wstring& base, const PresetIdentity& id) const
{
    std::wstring root = GetPresetsRootDir();
    std::wstring candidate = base;
    int suffix = 1;

    for (;;)
    {
        std::wstring folderPath = root + L"\\" + candidate;
        if (GetFileAttributesW(folderPath.c_str()) == INVALID_FILE_ATTRIBUTES)
            return candidate;

        PresetIdentity existing;
        std::wstring existingDisplay;
        if (ReadMeta(folderPath, existing, existingDisplay) && IdentityEquals(existing, id))
            return candidate;

        ++suffix;
        candidate = base + L"_" + std::to_wstring(suffix);
    }
}

std::vector<PresetEntry> PresetsManager::ListPresets() const
{
    std::vector<PresetEntry> result;
    std::wstring root = GetPresetsRootDir();
    std::wstring searchPath = root + L"\\*";

    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(searchPath.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return result;

    do
    {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;

        std::wstring folderPath = root + L"\\" + fd.cFileName;
        PresetEntry entry;
        entry.folderName = fd.cFileName;
        if (ReadMeta(folderPath, entry.identity, entry.displayName))
        {
            if (entry.displayName.empty())
                entry.displayName = entry.folderName;
            result.push_back(entry);
        }
    } while (FindNextFileW(hFind, &fd));
    FindClose(hFind);

    return result;
}

PresetEntry PresetsManager::FindPresetForIdentity(const PresetIdentity& id) const
{
    for (auto& e : ListPresets())
    {
        if (IdentityEquals(e.identity, id))
            return e;
    }
    return PresetEntry{};
}

bool PresetsManager::LoadPresetConfig(const std::wstring& folderName, Dlss5Config& outCfg) const
{
    std::wstring cfgPath = GetPresetsRootDir() + L"\\" + folderName + L"\\config.ini";
    if (GetFileAttributesW(cfgPath.c_str()) == INVALID_FILE_ATTRIBUTES) return false;

    const wchar_t* sec = L"Preset";
    wchar_t buf[64] = {};

    outCfg.style  = GetPrivateProfileIntW(sec, L"Style", 0, cfgPath.c_str());
    outCfg.preset = GetPrivateProfileIntW(sec, L"Preset", 0, cfgPath.c_str());

    GetPrivateProfileStringW(sec, L"Intensity", L"1.0", buf, _countof(buf), cfgPath.c_str());
    outCfg.intensity = static_cast<float>(_wtof(buf));

    GetPrivateProfileStringW(sec, L"BoostFactor", L"1.0", buf, _countof(buf), cfgPath.c_str());
    outCfg.boostFactor = static_cast<float>(_wtof(buf));

    GetPrivateProfileStringW(sec, L"LocalStructure", L"1.0", buf, _countof(buf), cfgPath.c_str());
    outCfg.localStructure = static_cast<float>(_wtof(buf));

    GetPrivateProfileStringW(sec, L"LocalTone", L"1.0", buf, _countof(buf), cfgPath.c_str());
    outCfg.localTone = static_cast<float>(_wtof(buf));

    GetPrivateProfileStringW(sec, L"SkinStructure", L"-1.0", buf, _countof(buf), cfgPath.c_str());
    outCfg.skinStructure = static_cast<float>(_wtof(buf));

    outCfg.resolutionScale = GetPrivateProfileIntW(sec, L"ResolutionScale", 100, cfgPath.c_str());
    outCfg.passCount       = GetPrivateProfileIntW(sec, L"PassCount", 1, cfgPath.c_str());

    GetPrivateProfileStringW(sec, L"PassFalloff", L"1.0", buf, _countof(buf), cfgPath.c_str());
    outCfg.passFalloff = static_cast<float>(_wtof(buf));

    outCfg.useAutoMask = (GetPrivateProfileIntW(sec, L"UseAutoMask", 1, cfgPath.c_str()) != 0);
    outCfg.opticalFlow = (GetPrivateProfileIntW(sec, L"OpticalFlow", 1, cfgPath.c_str()) != 0);
    outCfg.splitScreen  = (GetPrivateProfileIntW(sec, L"SplitScreen", 0, cfgPath.c_str()) != 0);

    GetPrivateProfileStringW(sec, L"SplitPos", L"0.5", buf, _countof(buf), cfgPath.c_str());
    outCfg.splitPos = static_cast<float>(_wtof(buf));

    return true;
}

std::wstring PresetsManager::SavePreset(const PresetIdentity& id, const Dlss5Config& cfg)
{
    std::wstring root = GetPresetsRootDir();

    PresetEntry existing = FindPresetForIdentity(id);
    std::wstring folderName = existing.folderName;

    if (folderName.empty())
    {
        std::wstring base = MakeCosmeticFolderName(id);
        folderName = DedupeFolderName(base, id);
    }

    std::wstring folderPath = root + L"\\" + folderName;
    if (GetFileAttributesW(folderPath.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        if (!CreateDirectoryW(folderPath.c_str(), nullptr))
        {
            DLSS_Log("[Presets] Klasor olusturulamadi: %ls", folderPath.c_str());
            return L"";
        }
    }

    WriteMeta(folderPath, id, MakeDisplayName(id));

    std::wstring cfgPath = folderPath + L"\\config.ini";
    EnsureBom(cfgPath);

    auto writeInt = [&](const wchar_t* key, int val) {
        wchar_t b[32];
        swprintf_s(b, L"%d", val);
        WritePrivateProfileStringW(L"Preset", key, b, cfgPath.c_str());
    };
    auto writeFloat = [&](const wchar_t* key, float val) {
        wchar_t b[32];
        swprintf_s(b, L"%.2f", val);
        WritePrivateProfileStringW(L"Preset", key, b, cfgPath.c_str());
    };

    writeInt(L"Style", cfg.style);
    writeInt(L"Preset", cfg.preset);
    writeFloat(L"Intensity", cfg.intensity);
    writeFloat(L"BoostFactor", cfg.boostFactor);
    writeFloat(L"LocalStructure", cfg.localStructure);
    writeFloat(L"LocalTone", cfg.localTone);
    writeFloat(L"SkinStructure", cfg.skinStructure);
    writeInt(L"ResolutionScale", cfg.resolutionScale);
    writeInt(L"PassCount", cfg.passCount);
    writeFloat(L"PassFalloff", cfg.passFalloff);
    writeInt(L"UseAutoMask", cfg.useAutoMask ? 1 : 0);
    writeInt(L"OpticalFlow", cfg.opticalFlow ? 1 : 0);
    writeInt(L"SplitScreen", cfg.splitScreen ? 1 : 0);
    writeFloat(L"SplitPos", cfg.splitPos);

    return folderName;
}

bool PresetsManager::DeletePreset(const std::wstring& folderName)
{
    if (folderName.empty()) return false;
    std::wstring folderPath = GetPresetsRootDir() + L"\\" + folderName;

    DeleteFileW((folderPath + L"\\config.ini").c_str());
    DeleteFileW((folderPath + L"\\meta.ini").c_str());

    return RemoveDirectoryW(folderPath.c_str()) != 0;
}
