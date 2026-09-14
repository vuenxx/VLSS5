#include "ConfigManager.h"
#include <shlwapi.h>
#include <cstdio>

#pragma comment(lib, "shlwapi.lib")

std::wstring Dlss5Config::FormatKey(UINT vk, UINT mod)
{
    std::wstring result;
    if (mod & MOD_CONTROL) result += L"Ctrl+";
    if (mod & MOD_ALT)     result += L"Alt+";
    if (mod & MOD_SHIFT)   result += L"Shift+";

    switch (vk)
    {
    case VK_INSERT:   result += L"INSERT";   break;
    case VK_HOME:     result += L"HOME";     break;
    case VK_END:      result += L"END";      break;
    case VK_PRIOR:    result += L"PGUP";     break;
    case VK_NEXT:     result += L"PGDN";     break;
    case VK_F1:       result += L"F1";       break;
    case VK_F2:       result += L"F2";       break;
    case VK_F3:       result += L"F3";       break;
    case VK_F4:       result += L"F4";       break;
    case VK_F5:       result += L"F5";       break;
    case VK_F6:       result += L"F6";       break;
    case VK_F7:       result += L"F7";       break;
    case VK_F8:       result += L"F8";       break;
    case VK_F9:       result += L"F9";       break;
    case VK_F10:      result += L"F10";      break;
    case VK_F11:      result += L"F11";      break;
    case VK_F12:      result += L"F12";      break;
    case VK_DELETE:   result += L"DELETE";   break;
    case VK_SPACE:    result += L"SPACE";    break;
    case VK_OEM_3:    result += L"~";        break;
    default:
        if (vk >= 'A' && vk <= 'Z')
        {
            result += static_cast<wchar_t>(vk);
        }
        else if (vk >= '0' && vk <= '9')
        {
            result += static_cast<wchar_t>(vk);
        }
        else
        {
            wchar_t hex[16];
            swprintf_s(hex, L"VK_0x%02X", vk);
            result += hex;
        }
        break;
    }
    return result;
}

std::wstring Dlss5Config::FormatHotkey() const
{
    return FormatKey(settingsVk, settingsMod);
}

ConfigManager& ConfigManager::Get()
{
    static ConfigManager s_instance;
    return s_instance;
}

std::wstring ConfigManager::GetIniPath() const
{
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    PathRemoveFileSpecW(path);
    PathCombineW(path, path, L"vlss5_config.ini");
    return path;
}

void ConfigManager::Load()
{
    std::wstring ini = GetIniPath();

    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        Save();
        return;
    }

    const wchar_t* sec = L"VLSS5";
    wchar_t testBuf[16] = {};
    GetPrivateProfileStringW(L"VLSS5", L"Intensity", L"", testBuf, _countof(testBuf), ini.c_str());
    if (testBuf[0] == L'\0')
    {
        GetPrivateProfileStringW(L"DLSS5", L"Intensity", L"", testBuf, _countof(testBuf), ini.c_str());
        if (testBuf[0] != L'\0') sec = L"DLSS5";
    }

    m_config.preset = GetPrivateProfileIntW(sec, L"Preset", 0, ini.c_str());
    if (m_config.preset < 0 || m_config.preset > 3) m_config.preset = 0;

    m_config.style = GetPrivateProfileIntW(sec, L"Style", 0, ini.c_str());
    if (m_config.style < 0 || m_config.style > 2) m_config.style = 0;

    wchar_t buf[64] = {};
    GetPrivateProfileStringW(sec, L"Intensity", L"1.0", buf, _countof(buf), ini.c_str());
    m_config.intensity = static_cast<float>(_wtof(buf));
    if (m_config.intensity < 0.0f) m_config.intensity = 0.0f;
    if (m_config.intensity > 2.0f) m_config.intensity = 2.0f;

    GetPrivateProfileStringW(sec, L"LocalStructure", L"1.0", buf, _countof(buf), ini.c_str());
    m_config.localStructure = static_cast<float>(_wtof(buf));
    if (m_config.localStructure < 0.0f) m_config.localStructure = 0.0f;
    if (m_config.localStructure > 2.0f) m_config.localStructure = 2.0f;

    GetPrivateProfileStringW(sec, L"LocalTone", L"1.0", buf, _countof(buf), ini.c_str());
    m_config.localTone = static_cast<float>(_wtof(buf));
    if (m_config.localTone < 0.0f) m_config.localTone = 0.0f;
    if (m_config.localTone > 2.0f) m_config.localTone = 2.0f;

    GetPrivateProfileStringW(sec, L"SkinStructure", L"-1.0", buf, _countof(buf), ini.c_str());
    m_config.skinStructure = static_cast<float>(_wtof(buf));
    if (m_config.skinStructure < -1.0f) m_config.skinStructure = -1.0f;
    if (m_config.skinStructure > 2.0f)  m_config.skinStructure = 2.0f;

    m_config.useAutoMask = (GetPrivateProfileIntW(sec, L"UseAutoMask", 1, ini.c_str()) != 0);

    m_config.resolutionScale = GetPrivateProfileIntW(sec, L"ResolutionScale", 100, ini.c_str());
    if (m_config.resolutionScale < 50)  m_config.resolutionScale = 50;
    if (m_config.resolutionScale > 100) m_config.resolutionScale = 100;

    m_config.temporalStabilizer = (GetPrivateProfileIntW(sec, L"TemporalStabilizer", 0, ini.c_str()) != 0);
    m_config.opticalFlow        = (GetPrivateProfileIntW(sec, L"OpticalFlow", 1, ini.c_str()) != 0);

    GetPrivateProfileStringW(sec, L"BoostFactor", L"1.0", buf, _countof(buf), ini.c_str());
    m_config.boostFactor = static_cast<float>(_wtof(buf));
    if (m_config.boostFactor < 1.0f) m_config.boostFactor = 1.0f;
    if (m_config.boostFactor > 2.5f) m_config.boostFactor = 2.5f;

    m_config.splitScreen = (GetPrivateProfileIntW(sec, L"SplitScreen", 0, ini.c_str()) != 0);
    m_config.directFlip  = (GetPrivateProfileIntW(sec, L"DirectFlip", 0, ini.c_str()) != 0);
    m_config.fullscreenStretch = (GetPrivateProfileIntW(sec, L"FullscreenStretch", 0, ini.c_str()) != 0);

    GetPrivateProfileStringW(sec, L"SplitPos", L"0.5", buf, _countof(buf), ini.c_str());
    m_config.splitPos = static_cast<float>(_wtof(buf));
    if (m_config.splitPos < 0.0f) m_config.splitPos = 0.0f;
    if (m_config.splitPos > 1.0f) m_config.splitPos = 1.0f;

    m_config.settingsVk  = static_cast<UINT>(GetPrivateProfileIntW(L"Hotkeys", L"SettingsVk", VK_INSERT, ini.c_str()));
    m_config.settingsMod = static_cast<UINT>(GetPrivateProfileIntW(L"Hotkeys", L"SettingsMod", 0, ini.c_str()));

    m_config.vkFgIndicator = static_cast<UINT>(GetPrivateProfileIntW(L"Hotkeys", L"VkFgIndicator", VK_F7, ini.c_str()));
    m_config.vkFocus       = static_cast<UINT>(GetPrivateProfileIntW(L"Hotkeys", L"VkFocus", VK_F8, ini.c_str()));
    m_config.vkFps         = static_cast<UINT>(GetPrivateProfileIntW(L"Hotkeys", L"VkFps", VK_F9, ini.c_str()));
    m_config.vkToggleVlss  = static_cast<UINT>(GetPrivateProfileIntW(L"Hotkeys", L"VkToggleVlss", VK_F10, ini.c_str()));
    m_config.vkCalib       = static_cast<UINT>(GetPrivateProfileIntW(L"Hotkeys", L"VkCalib", VK_F2, ini.c_str()));
    m_config.vkStart       = static_cast<UINT>(GetPrivateProfileIntW(L"Hotkeys", L"VkStart", 'S', ini.c_str()));
    m_config.modStart      = static_cast<UINT>(GetPrivateProfileIntW(L"Hotkeys", L"ModStart", MOD_ALT, ini.c_str()));

    wchar_t gpuBuf[256] = {};
    GetPrivateProfileStringW(L"Hardware", L"SelectedGpu", L"Auto", gpuBuf, _countof(gpuBuf), ini.c_str());
    m_config.selectedGpu = (gpuBuf[0] != L'\0') ? gpuBuf : L"Auto";

    wchar_t rtssBuf[MAX_PATH] = {};
    GetPrivateProfileStringW(L"Paths", L"RtssDirectory", L"", rtssBuf, _countof(rtssBuf), ini.c_str());
    m_config.rtssDirectory = rtssBuf;
}

void ConfigManager::Save()
{
    std::wstring ini = GetIniPath();

    auto writeStr = [&](const wchar_t* sec, const wchar_t* key, const wchar_t* val) {
        WritePrivateProfileStringW(sec, key, val, ini.c_str());
    };
    auto writeInt = [&](const wchar_t* sec, const wchar_t* key, int val) {
        wchar_t buf[32];
        swprintf_s(buf, L"%d", val);
        WritePrivateProfileStringW(sec, key, buf, ini.c_str());
    };
    auto writeFloat = [&](const wchar_t* sec, const wchar_t* key, float val) {
        wchar_t buf[32];
        swprintf_s(buf, L"%.2f", val);
        WritePrivateProfileStringW(sec, key, buf, ini.c_str());
    };

    writeInt(L"VLSS5", L"Preset", m_config.preset);
    writeInt(L"VLSS5", L"Style", m_config.style);
    writeFloat(L"VLSS5", L"Intensity", m_config.intensity);
    writeFloat(L"VLSS5", L"LocalStructure", m_config.localStructure);
    writeFloat(L"VLSS5", L"LocalTone", m_config.localTone);
    writeFloat(L"VLSS5", L"SkinStructure", m_config.skinStructure);
    writeInt(L"VLSS5", L"UseAutoMask", m_config.useAutoMask ? 1 : 0);
    writeInt(L"VLSS5", L"ResolutionScale", m_config.resolutionScale);
    writeInt(L"VLSS5", L"TemporalStabilizer", m_config.temporalStabilizer ? 1 : 0);
    writeInt(L"VLSS5", L"OpticalFlow", m_config.opticalFlow ? 1 : 0);
    writeFloat(L"VLSS5", L"BoostFactor", m_config.boostFactor);
    writeInt(L"VLSS5", L"SplitScreen", m_config.splitScreen ? 1 : 0);
    writeInt(L"VLSS5", L"DirectFlip",  m_config.directFlip  ? 1 : 0);
    writeInt(L"VLSS5", L"FullscreenStretch", m_config.fullscreenStretch ? 1 : 0);
    writeFloat(L"VLSS5", L"SplitPos", m_config.splitPos);

    writeInt(L"Hotkeys", L"SettingsVk", static_cast<int>(m_config.settingsVk));
    writeInt(L"Hotkeys", L"SettingsMod", static_cast<int>(m_config.settingsMod));

    writeInt(L"Hotkeys", L"VkFgIndicator", static_cast<int>(m_config.vkFgIndicator));
    writeInt(L"Hotkeys", L"VkFocus", static_cast<int>(m_config.vkFocus));
    writeInt(L"Hotkeys", L"VkFps", static_cast<int>(m_config.vkFps));
    writeInt(L"Hotkeys", L"VkToggleVlss", static_cast<int>(m_config.vkToggleVlss));
    writeInt(L"Hotkeys", L"VkCalib", static_cast<int>(m_config.vkCalib));
    writeInt(L"Hotkeys", L"VkStart", static_cast<int>(m_config.vkStart));
    writeInt(L"Hotkeys", L"ModStart", static_cast<int>(m_config.modStart));

    writeStr(L"Hardware", L"SelectedGpu", m_config.selectedGpu.c_str());
    writeStr(L"Paths", L"RtssDirectory", m_config.rtssDirectory.c_str());
}
