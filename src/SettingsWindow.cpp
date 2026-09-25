#include "SettingsWindow.h"
#include "App.h"
#include "WindowEnumerator.h"
#include "../third_party/json/json.hpp"
#include <algorithm>

using json = nlohmann::json;

std::unique_ptr<WebViewHost>          SettingsWindow::s_host;
HINSTANCE                             SettingsWindow::s_hInstance = nullptr;
SettingsWindow::ConfigChangedCallback SettingsWindow::s_callback  = nullptr;
App*                                  SettingsWindow::s_app       = nullptr;
std::vector<PresetEntry>              SettingsWindow::s_presetSourceCache;

// ---------------------------------------------------------------------------
// ForceForeground
//   Oyun foreground'dayken SetForegroundWindow sessizce yok sayilir (foreground
//   lock). Bu yuzden hedef thread'e AttachThreadInput ile baglanip kilit
//   timeout'unu gecici olarak sifirliyoruz. Amac pencereyi one almak DEGIL;
//   oyunun gercekten WM_ACTIVATEAPP/WM_KILLFOCUS alip mouse'u KENDI birakmasi.
// ---------------------------------------------------------------------------
static void ForceForeground(HWND hwnd)
{
    if (!hwnd) return;

    HWND  fg    = GetForegroundWindow();
    DWORD fgTid = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    DWORD myTid = GetCurrentThreadId();

    // KRITIK: Windows'un "foreground lock" korumasini gecici olarak devre
    // disi birak. Bu yapilmazsa SetForegroundWindow bazen SESSIZCE yok
    // sayilir (pencere one gelmez) VE -- daha kotusu -- oyun WM_ACTIVATEAPP/
    // WM_KILLFOCUS'u TUTARSIZ sekilde alir, bu da input state'inin (tuslar,
    // fare yakalama) temiz gecis yapmamasina, yani oyun icinde "sacma"
    // hareketlere yol acar. (Orijinal Win32 SettingsWindow'da vardi, WebView2
    // portunda yanlislikla atlanmisti.)
    DWORD lockTimeout = 0;
    SystemParametersInfoW(SPI_GETFOREGROUNDLOCKTIMEOUT, 0, &lockTimeout, 0);
    SystemParametersInfoW(SPI_SETFOREGROUNDLOCKTIMEOUT, 0,
                          reinterpret_cast<PVOID>(0), SPIF_SENDCHANGE);

    const bool attached = (fgTid != 0 && fgTid != myTid &&
                           AttachThreadInput(myTid, fgTid, TRUE) != 0);

    ShowWindow(hwnd, SW_SHOW);
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    BringWindowToTop(hwnd);
    SetForegroundWindow(hwnd);
    SetActiveWindow(hwnd);
    SetFocus(hwnd);

    if (attached)
        AttachThreadInput(myTid, fgTid, FALSE);

    SystemParametersInfoW(SPI_SETFOREGROUNDLOCKTIMEOUT, 0,
                          reinterpret_cast<PVOID>(static_cast<UINT_PTR>(lockTimeout)),
                          SPIF_SENDCHANGE);

    ClipCursor(nullptr);
}

void SettingsWindow::Initialize(HINSTANCE hInstance)
{
    s_hInstance = hInstance;
    WebViewHost::EnsureEnvironment(hInstance);
}

// Su an gercek bir yakalama hedefi (pencere veya "Tum Ekran" monitoru) var mi?
static bool SettingsWindowHasActiveTarget(App* app)
{
    return app && (app->GetTargetHwnd() != nullptr || app->IsDesktopMode());
}

void SettingsWindow::AutoSaveActivePreset()
{
    if (!s_app) return;

    PresetIdentity id;
    if (s_app->IsDesktopMode())
    {
        id.isMonitor     = true;
        id.monitorDevice = WindowEnumerator::ResolveMonitorDeviceName(s_app->GetTargetMonitor());
        if (id.monitorDevice.empty()) return;
    }
    else if (s_app->GetTargetHwnd())
    {
        id.isMonitor   = false;
        id.exeFullPath = WindowEnumerator::ResolveExeFullPath(s_app->GetTargetHwnd());
        if (id.exeFullPath.empty()) return;

        wchar_t titleBuf[256] = {};
        GetWindowTextW(s_app->GetTargetHwnd(), titleBuf, _countof(titleBuf));
        id.windowTitle = titleBuf;
    }
    else
    {
        return;
    }

    PresetsManager::Get().SavePreset(id, ConfigManager::Get().Config());
}

// Convenience: narrow std::wstring -> UTF-8 std::string (json string alanlari icin).
static std::string ToUtf8(const std::wstring& w)
{
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(len > 0 ? len - 1 : 0, '\0');
    if (len > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), len, nullptr, nullptr);
    return out;
}
static std::wstring FromUtf8(const std::string& s)
{
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring out(len > 0 ? len - 1 : 0, L'\0');
    if (len > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), len);
    return out;
}

void SettingsWindow::PushConfigToJs()
{
    if (!s_host) return;
    const auto& cfg = ConfigManager::Get().Config();

    json data;
    data["style"]           = cfg.style;
    data["preset"]          = cfg.preset;
    data["intensity"]       = cfg.intensity;
    data["boostFactor"]     = cfg.boostFactor;
    data["localStructure"]  = cfg.localStructure;
    data["localTone"]       = cfg.localTone;
    data["skinStructure"]   = cfg.skinStructure;
    data["resolutionScale"] = cfg.resolutionScale;
    data["passCount"]       = cfg.passCount;
    data["passFalloff"]     = cfg.passFalloff;
    data["useAutoMask"]     = cfg.useAutoMask;
    data["opticalFlow"]     = cfg.opticalFlow;
    data["splitScreen"]     = cfg.splitScreen;
    data["splitPos"]        = cfg.splitPos;

    json msg;
    msg["type"] = "config";
    msg["data"] = data;
    s_host->PostJson(FromUtf8(msg.dump()));
}

void SettingsWindow::PushPresetListToJs()
{
    if (!s_host) return;

    s_presetSourceCache = PresetsManager::Get().ListPresets();

    json arr = json::array();
    for (auto& e : s_presetSourceCache)
    {
        json item;
        item["folder"]      = ToUtf8(e.folderName);
        item["displayName"] = ToUtf8(e.displayName);
        arr.push_back(item);
    }

    json msg;
    msg["type"] = "presetList";
    msg["data"] = arr;
    s_host->PostJson(FromUtf8(msg.dump()));
}

void SettingsWindow::OnWebMessage(const std::wstring& jsonStr)
{
    json msg;
    try { msg = json::parse(ToUtf8(jsonStr)); }
    catch (...) { return; }

    try
    {
    std::string cmd = msg.value("cmd", "");

    if (cmd == "close")
    {
        Hide();
    }
    else if (cmd == "getConfig")
    {
        PushConfigToJs();
    }
    else if (cmd == "listPresets")
    {
        PushPresetListToJs();
    }
    else if (cmd == "setConfig")
    {
        if (!msg.contains("data")) return;
        auto& d = msg["data"];
        auto& cfg = ConfigManager::Get().Config();

        cfg.style           = d.value("style", cfg.style);
        cfg.preset          = d.value("preset", cfg.preset);
        cfg.intensity       = d.value("intensity", cfg.intensity);
        cfg.boostFactor     = std::clamp(d.value("boostFactor", cfg.boostFactor), 1.0f, 2.5f);
        cfg.localStructure  = d.value("localStructure", cfg.localStructure);
        cfg.localTone       = d.value("localTone", cfg.localTone);
        cfg.skinStructure   = d.value("skinStructure", cfg.skinStructure);
        if (cfg.skinStructure <= -0.99f) cfg.skinStructure = -1.0f;
        cfg.resolutionScale = d.value("resolutionScale", cfg.resolutionScale);
        cfg.passCount       = std::clamp(d.value("passCount", cfg.passCount), 1, 4);
        cfg.passFalloff     = std::clamp(d.value("passFalloff", cfg.passFalloff), 0.25f, 1.0f);
        cfg.useAutoMask     = d.value("useAutoMask", cfg.useAutoMask);
        cfg.opticalFlow     = d.value("opticalFlow", cfg.opticalFlow);
        cfg.splitScreen     = d.value("splitScreen", cfg.splitScreen);
        cfg.splitPos        = std::clamp(d.value("splitPos", cfg.splitPos), 0.0f, 1.0f);

        if (SettingsWindowHasActiveTarget(s_app))
        {
            // Aktif bir hedef varken bu pencere o OYUNUN ayarlarini duzenliyor --
            // degerler SADECE onun on ayarina yazilir.
            AutoSaveActivePreset();
        }
        else
        {
            ConfigManager::Get().Save();
        }

        if (s_callback) s_callback(cfg);
    }
    else if (cmd == "loadPreset")
    {
        std::string folder = msg.value("folder", "");
        if (folder.empty()) return;

        Dlss5Config loaded;
        if (PresetsManager::Get().LoadPresetConfig(FromUtf8(folder), loaded))
        {
            auto& cfg = ConfigManager::Get().Config();
            cfg.style           = loaded.style;
            cfg.preset          = loaded.preset;
            cfg.intensity       = loaded.intensity;
            cfg.boostFactor     = loaded.boostFactor;
            cfg.localStructure  = loaded.localStructure;
            cfg.localTone       = loaded.localTone;
            cfg.skinStructure   = loaded.skinStructure;
            cfg.resolutionScale = loaded.resolutionScale;
            cfg.passCount       = loaded.passCount;
            cfg.passFalloff     = loaded.passFalloff;
            cfg.useAutoMask     = loaded.useAutoMask;
            cfg.opticalFlow     = loaded.opticalFlow;
            cfg.splitScreen     = loaded.splitScreen;
            cfg.splitPos        = loaded.splitPos;

            PushConfigToJs();
            if (s_callback) s_callback(cfg);

            if (SettingsWindowHasActiveTarget(s_app))
                AutoSaveActivePreset();
            else
                ConfigManager::Get().Save();
        }
    }
    }
    catch (...) { /* Hatali/beklenmedik JSON alani -- bu mesaji yoksay, uygulamayi cokertme. */ }
}

static std::wstring ExeDirWebFolder(const wchar_t* subfolder)
{
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir = path;
    size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) dir = dir.substr(0, slash);
    return dir + L"\\web\\" + subfolder;
}

void SettingsWindow::Show(HWND parent)
{
    if (!s_host)
    {
        s_host = std::make_unique<WebViewHost>();
        s_host->CreatePopup(parent, L"VLSS5 Nöral Yapılandırma",
            L"vlss5.settings", ExeDirWebFolder(L"settings").c_str(), L"index.html",
            460, 900, &SettingsWindow::OnWebMessage);
    }
    else
    {
        s_host->Show();
    }

    ForceForeground(s_host->GetHwnd());
    PushPresetListToJs();
    PushConfigToJs();
}

void SettingsWindow::Hide()
{
    if (s_host)
    {
        s_host->Hide();
        ClipCursor(nullptr);
    }
}

void SettingsWindow::Toggle(HWND parent)
{
    if (s_host && s_host->IsOpen())
        Hide();
    else
        Show(parent);
}

bool SettingsWindow::IsOpen()
{
    return s_host && s_host->IsOpen();
}

HWND SettingsWindow::GetHwnd()
{
    return s_host ? s_host->GetHwnd() : nullptr;
}

void SettingsWindow::SetOnConfigChanged(ConfigChangedCallback cb)
{
    s_callback = cb;
}

void SettingsWindow::SetAppInstance(App* app)
{
    s_app = app;
}
