#pragma once
#include "Common.h"
#include "PresetsManager.h"
#include "WebViewHost.h"
#include "../third_party/json/json.hpp"
#include <memory>

// ---------------------------------------------------------------------------
// PresetsWindow
//   Ana sayfadaki "ON AYARLAR" penceresi: kaydedilmis tum on ayarlari listeler,
//   secileni onayla siler. WebView2 tabanli HTML/CSS/JS arayuz kullanir.
// ---------------------------------------------------------------------------
class PresetsWindow
{
public:
    static void Initialize(HINSTANCE hInstance);
    static void Show(HWND parent = nullptr);
    static void Hide();
    static bool IsOpen();
    static HWND GetHwnd();

private:
    static void OnWebMessage(const std::wstring& json);
    static void DispatchMessage(const nlohmann::json& msg);
    static void PushListToJs();

    static std::unique_ptr<WebViewHost> s_host;
    static HINSTANCE s_hInstance;
    static std::vector<PresetEntry> s_cache;
};
