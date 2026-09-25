#pragma once
#include "Common.h"
#include "ConfigManager.h"
#include "PresetsManager.h"
#include "WebViewHost.h"
#include <functional>
#include <memory>

class App;

class SettingsWindow
{
public:
    using ConfigChangedCallback = std::function<void(const Dlss5Config&)>;

    static void Initialize(HINSTANCE hInstance);
    static void Show(HWND parent = nullptr);
    static void Hide();
    static void Toggle(HWND parent = nullptr);
    static bool IsOpen();
    static HWND GetHwnd();

    static void SetOnConfigChanged(ConfigChangedCallback cb);

    // Aktif yakalama hedefini (App uzerinden) bilmesi icin -- her ayar
    // degisikliginde ilgili on ayara otomatik kaydetmek bu baglantiya dayanir.
    static void SetAppInstance(App* app);

private:
    // Aktif hedefin kimligine (tam exe yolu / monitor cihazi) gore diske
    // SESSIZCE yazar (onay istemez, hedef yoksa no-op). Her ayar
    // degisikliginde otomatik cagrilir -- bkz. OnSettingChanged.
    static void AutoSaveActivePreset();

    // JS'ten gelen JSON komutlarini isler (bkz. WebViewHost::MessageHandler).
    static void OnWebMessage(const std::wstring& json);

    static void PushConfigToJs();
    static void PushPresetListToJs();

    static std::unique_ptr<WebViewHost> s_host;
    static HINSTANCE                    s_hInstance;
    static ConfigChangedCallback        s_callback;
    static App*                         s_app;
    static std::vector<PresetEntry>     s_presetSourceCache;
};
