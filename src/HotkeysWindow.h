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

    // Ayri "Tuşları Değiştir" pop-up penceresi (web/hotkeys/*, artik UI'dan
    // erisilmiyor ama sinif hala bu akisi destekliyor) icin "bas -> tusa bas"
    // yakalama akisi.
    static void StartKeybindCapture(int hotkeyId);
    static void EndKeybindCapture(bool save, UINT vk, UINT mod);

    // Ana pencerenin "Tuşları Değiştir" paneli artik "tusa bas ve bekle"
    // YERINE bir dropdown'dan (F1, F2, ... gibi) secim yapiyor -- bekleyecek
    // bir sey olmadigi icin StartKeybindCapture/EndKeybindCapture'daki
    // s_rebindingKey durum makinesine hic girmeden, secilen vk/mod'u DOGRUDAN
    // yazan bu yol kullaniliyor (bkz. web/main/app.js, Main.cpp "hotkeysSetKey").
    static void SetKey(int hotkeyId, UINT vk, UINT mod);

private:
    static void ApplyAndNotify(int hotkeyId, UINT vk, UINT mod);
    static void OnWebMessage(const std::wstring& json);
    static void PushHotkeysToJs();

    static std::unique_ptr<WebViewHost> s_host;
    static HINSTANCE s_hInstance;

    static bool s_rebindingKey;
    static int s_currentRebindId; // 0=Settings, 2=Focus, 3=FPS, 4=VLSS, 5=Calib, 6=Start, 7=DismissWarning

    static KeyCapturedCallback s_externalCallback;
};
