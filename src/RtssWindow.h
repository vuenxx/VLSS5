#pragma once
#include "Common.h"
#include "ConfigManager.h"
#include "RTSSManager.h"
#include "WebViewHost.h"
#include "../third_party/json/json.hpp"
#include <memory>

class RtssWindow
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
    static void PushDirToJs();
    static void PushProfilesToJs();

    static std::unique_ptr<WebViewHost> s_host;
    static HINSTANCE s_hInstance;
};
