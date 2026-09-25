#pragma once
#include "Common.h"
#include <functional>
#include <wrl.h>
#include "WebView2.h"

// ---------------------------------------------------------------------------
// WebViewHost
//   Ortak WebView2 tabanli borderless WS_POPUP pencere host'u. SettingsWindow,
//   HotkeysWindow, RtssWindow ve PresetsWindow'un dorduzu de bunu kullanir.
//
//   - Tek, paylasimli ICoreWebView2Environment (%LOCALAPPDATA%\VLSS5\WebView2UserData).
//   - HTML dosyalari SetVirtualHostNameToFolderMapping ile file:// KULLANMADAN,
//     https://<virtualHostName>/<startPage> adresinden yuklenir.
//   - Native <-> JS mesajlasmasi PostWebMessageAsJson / WebMessageReceived
//     (ham JSON string) uzerinden -- AddHostObjectToScript KULLANILMAZ.
//   - WM_CLOSE / JS'ten gelen {cmd:"close"} penceriyi YOK ETMEZ, gizler (Hide())
//     -- boylece mevcut 4 pencerenin static-singleton Show()/Hide() deseni
//     degismeden korunur.
// ---------------------------------------------------------------------------
class WebViewHost
{
public:
    using MessageHandler = std::function<void(const std::wstring& json)>;

    // Paylasimli CoreWebView2Environment'i bir kez olusturur. Main.cpp'nin
    // basinda (veya ilk pencere acilmadan once) bir kez cagrilmali.
    static bool EnsureEnvironment(HINSTANCE hInstance);

    WebViewHost() = default;
    ~WebViewHost();

    WebViewHost(const WebViewHost&) = delete;
    WebViewHost& operator=(const WebViewHost&) = delete;

    // Borderless WS_POPUP pencere olusturur ve icine WebView2 kontrolunu
    // yerlestirir. virtualHostFolder MUTLAK bir klasor yolu olmali (orn.
    // exe'nin yanindaki "web\\settings"). startPage bu klasore GORECELI
    // (orn. L"index.html").
    bool CreatePopup(HWND parent, const wchar_t* windowTitle,
                      const wchar_t* virtualHostName, const wchar_t* virtualHostFolder,
                      const wchar_t* startPage, int width, int height,
                      MessageHandler onMessage);

    // "Gomulu mod": kendi HWND'sini yaratmaz, var olan bir ana pencerenin
    // istemci alanina WebView2 kontrolunu yerlestirir. Ana pencere (Main.cpp)
    // kendi WndProc'unu ve WM_SIZE isleyicisini korur; boyut degisince
    // Resize() cagirmasi gerekir. Show()/Hide()/IsOpen()/WM_CLOSE-gizle
    // semantigi bu modda kullanilmaz.
    bool CreateEmbedded(HWND existingParent, const wchar_t* virtualHostName,
                         const wchar_t* virtualHostFolder, const wchar_t* startPage,
                         MessageHandler onMessage);

    void Show();
    void Hide();
    bool IsOpen() const;
    HWND GetHwnd() const { return m_hwnd; }

    // Native -> JS: JSON string'i oldugu gibi window.chrome.webview'e postalar.
    void PostJson(const std::wstring& json);

    // Public disa acilmis yeniden-boyutlandirma (gomulu mod icin). CreatePopup
    // modunda da zararsizdir (HandleResize zaten m_hwnd'nin client rect'ini okur).
    void Resize(const RECT& clientRect);

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    void HandleResize();
    void FinishSetup(ICoreWebView2Controller* controller);

    HWND      m_hwnd      = nullptr;
    HINSTANCE m_hInstance = nullptr;
    // true ise m_hwnd baska birinin (Main.cpp) penceresidir -- destructor bunu
    // DestroyWindow ETMEMELI.
    bool      m_embedded  = false;

    Microsoft::WRL::ComPtr<ICoreWebView2Controller> m_controller;
    Microsoft::WRL::ComPtr<ICoreWebView2>           m_webview;
    EventRegistrationToken                          m_msgToken{};
    bool                                             m_msgTokenValid = false;

    MessageHandler m_onMessage;
    std::wstring   m_virtualHostName;
    std::wstring   m_virtualHostFolder;
    std::wstring   m_startPage;
    int            m_initialWidth  = 0;
    int            m_initialHeight = 0;
};
