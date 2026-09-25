#pragma once
#include "Common.h"
#include "ConfigManager.h"
#include "WebViewHost.h"
#include <functional>
#include <memory>

class HotkeysWindow
{
public:
    // Faz 3: hem gercek bir tus yakalandiginda (save=true) hem de iptal edildiginde
    // (Esc, save=false) cagrilir -- Main.cpp'nin ana pencere sekmesi (KENDI ayri
    // WebViewHost'u -- s_host DEGIL) bunu kullanarak hotkey pill'ini/sekme buton
    // etiketlerini gunceller. s_host varsa (popup acikken) ONA yapilan PostJson
    // cagrilari zaten degismeden calismaya devam eder; bu callback SADECE EK bir
    // bildirim kanalidir.
    using KeyCapturedCallback = std::function<void(bool save, UINT vk, UINT mod)>;

    static void Initialize(HINSTANCE hInstance);
    static void Show(HWND parent = nullptr);
    static void Hide();
    static bool IsOpen();
    static HWND GetHwnd();

    static void SetExternalKeyCapturedCallback(KeyCapturedCallback cb);

    // Faz 3: Main.cpp'nin ana pencere sekmesi WH_KEYBOARD_LL hook mekanizmasini
    // TEKRAR YAZMAK yerine dogrudan bu ikisini cagirir (bkz. plan section 4).
    static void StartKeybindCapture(int hotkeyId);
    static void EndKeybindCapture(bool save, UINT vk, UINT mod);

private:
    static LRESULT CALLBACK RebindKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);

    static void OnWebMessage(const std::wstring& json);
    static void PushHotkeysToJs();

    static std::unique_ptr<WebViewHost> s_host;
    static HINSTANCE s_hInstance;

    static bool s_rebindingKey;
    static HHOOK s_rebindHook;
    static int s_currentRebindId; // 0=Settings, 2=Focus, 3=FPS, 4=VLSS, 5=Calib, 6=Start, 7=DismissWarning

    static KeyCapturedCallback s_externalCallback;
};
