#include "WebViewHost.h"
#include <shlwapi.h>

#pragma comment(lib, "shlwapi.lib")

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace
{
    ComPtr<ICoreWebView2Environment> g_environment;
    bool                             g_environmentReady = false;

    // Basit bir modal-olmayan mesaj pompasi: WebView2'nin COM async
    // tamamlanma callback'leri (environment/controller olusturma) ancak
    // thread'in mesaj kuyrugu isleniyorsa cagrilir. Bu fonksiyon 'done'
    // true olana kadar (veya makul bir sinira kadar) mesajlari pompalar.
    void PumpUntil(bool& done)
    {
        MSG msg;
        // ~10 saniye guvenlik siniri (WebView2 Runtime kurulu degilse sonsuz
        // donguye girmeyelim).
        const ULONGLONG startTick = GetTickCount64();
        while (!done)
        {
            if (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            else
            {
                if (GetTickCount64() - startTick > 10000) break;
                MsgWaitForMultipleObjects(0, nullptr, FALSE, 50, QS_ALLINPUT);
            }
        }
    }

    std::wstring GetUserDataFolder()
    {
        wchar_t localAppData[MAX_PATH] = {};
        DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH);
        std::wstring base = (len > 0 && len < MAX_PATH) ? localAppData : L".";
        std::wstring dir = base + L"\\VLSS5\\WebView2UserData";
        CreateDirectoryW((base + L"\\VLSS5").c_str(), nullptr);
        CreateDirectoryW(dir.c_str(), nullptr);
        return dir;
    }
}

bool WebViewHost::EnsureEnvironment(HINSTANCE /*hInstance*/)
{
    if (g_environmentReady) return true;

    bool done = false;
    HRESULT createHr = E_FAIL;

    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, GetUserDataFolder().c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [&done, &createHr](HRESULT result, ICoreWebView2Environment* env) -> HRESULT
            {
                createHr = result;
                if (SUCCEEDED(result) && env)
                    g_environment = env;
                done = true;
                return S_OK;
            }).Get());

    if (FAILED(hr))
        return false;

    PumpUntil(done);

    g_environmentReady = SUCCEEDED(createHr) && g_environment;
    return g_environmentReady;
}

WebViewHost::~WebViewHost()
{
    if (m_webview && m_msgTokenValid)
        m_webview->remove_WebMessageReceived(m_msgToken);
    if (m_controller)
        m_controller->Close();
    // Gomulu modda m_hwnd bize ait degil (Main.cpp'nin kendi ana penceresi) --
    // onu yok etmek ana pencereyi ayagimizin altindan cekerdi.
    if (m_hwnd && !m_embedded)
        DestroyWindow(m_hwnd);
}

LRESULT CALLBACK WebViewHost::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    auto* self = reinterpret_cast<WebViewHost*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg)
    {
    case WM_NCCREATE:
    {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        break;
    }
    case WM_SIZE:
        if (self) self->HandleResize();
        return 0;
    case WM_CLOSE:
        // Yok etme -- gizle (mevcut 4 pencerenin Show()/Hide() singleton deseni).
        if (self) self->Hide();
        return 0;
    case WM_ACTIVATE:
        if (self && self->m_controller && LOWORD(wParam) != WA_INACTIVE)
            self->m_controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void WebViewHost::HandleResize()
{
    if (!m_controller || !m_hwnd) return;
    RECT rc;
    GetClientRect(m_hwnd, &rc);
    m_controller->put_Bounds(rc);
}

bool WebViewHost::CreatePopup(HWND parent, const wchar_t* windowTitle,
                               const wchar_t* virtualHostName, const wchar_t* virtualHostFolder,
                               const wchar_t* startPage, int width, int height,
                               MessageHandler onMessage)
{
    DLSS_Log("[WebViewHost] CreatePopup('%ls') cagrildi.", virtualHostName);

    if (m_hwnd)
    {
        Show();
        return true;
    }

    if (!g_environmentReady && !EnsureEnvironment(nullptr))
    {
        DLSS_Log("[WebViewHost] CreatePopup('%ls'): EnsureEnvironment basarisiz.", virtualHostName);
        return false;
    }

    m_onMessage          = std::move(onMessage);
    m_virtualHostName    = virtualHostName;
    m_virtualHostFolder  = virtualHostFolder;
    m_startPage          = startPage;
    m_initialWidth       = width;
    m_initialHeight      = height;

    static bool classRegistered = false;
    HINSTANCE hInst = reinterpret_cast<HINSTANCE>(GetModuleHandleW(nullptr));
    if (!classRegistered)
    {
        WNDCLASSEXW wc = {};
        wc.cbSize        = sizeof(WNDCLASSEXW);
        wc.style         = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc   = WndProc;
        wc.hInstance     = hInst;
        wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        wc.lpszClassName = L"VLSS5_WebViewHostClass";
        RegisterClassExW(&wc);
        classRegistered = true;
    }

    int x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;

    HWND owner = parent;
    if (owner && (GetWindowLongPtrW(owner, GWL_EXSTYLE) & WS_EX_NOACTIVATE))
        owner = nullptr;

    m_hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        L"VLSS5_WebViewHostClass", windowTitle,
        WS_POPUP | WS_VISIBLE,
        x, y, width, height,
        owner, nullptr, hInst, this);

    if (!m_hwnd)
    {
        DLSS_Log("[WebViewHost] CreatePopup('%ls'): CreateWindowExW basarisiz (LastError=%lu)", virtualHostName, GetLastError());
        return false;
    }

    BOOL useDarkMode = TRUE;
    DwmSetWindowAttribute(m_hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &useDarkMode, sizeof(useDarkMode));
    DWORD cornerPref = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(m_hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &cornerPref, sizeof(cornerPref));

    bool done = false;
    HRESULT createHr = E_FAIL;
    ComPtr<ICoreWebView2Controller> resultController;

    HRESULT hr = g_environment->CreateCoreWebView2Controller(
        m_hwnd,
        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            [&done, &createHr, &resultController](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT
            {
                createHr = result;
                if (SUCCEEDED(result) && controller)
                    resultController = controller;
                done = true;
                return S_OK;
            }).Get());

    if (FAILED(hr))
    {
        DLSS_Log("[WebViewHost] CreatePopup('%ls'): CreateCoreWebView2Controller cagrisi basarisiz (hr=0x%08X)", virtualHostName, hr);
        return false;
    }

    PumpUntil(done);

    if (FAILED(createHr) || !resultController)
    {
        DLSS_Log("[WebViewHost] CreatePopup('%ls'): controller olusturma tamamlanamadi (createHr=0x%08X, done=%d)", virtualHostName, createHr, done ? 1 : 0);
        return false;
    }

    DLSS_Log("[WebViewHost] CreatePopup('%ls'): controller basariyla olusturuldu, FinishSetup cagriliyor.", virtualHostName);
    FinishSetup(resultController.Get());
    return true;
}

bool WebViewHost::CreateEmbedded(HWND existingParent, const wchar_t* virtualHostName,
                                  const wchar_t* virtualHostFolder, const wchar_t* startPage,
                                  MessageHandler onMessage)
{
    if (m_hwnd)
        return true; // zaten kuruldu

    if (!existingParent)
        return false;

    if (!g_environmentReady && !EnsureEnvironment(nullptr))
        return false;

    m_onMessage         = std::move(onMessage);
    m_virtualHostName   = virtualHostName;
    m_virtualHostFolder = virtualHostFolder;
    m_startPage         = startPage;

    // Kendi penceremizi YARATMIYORUZ -- var olan ana pencereyi dogrudan
    // kullaniyoruz. WebViewHost::WndProc hic devreye girmez; Main.cpp kendi
    // WndProc'unu ve WM_SIZE -> Resize() yonlendirmesini kendisi yapar.
    m_hwnd     = existingParent;
    m_embedded = true;

    bool done = false;
    HRESULT createHr = E_FAIL;
    ComPtr<ICoreWebView2Controller> resultController;

    HRESULT hr = g_environment->CreateCoreWebView2Controller(
        m_hwnd,
        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            [&done, &createHr, &resultController](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT
            {
                createHr = result;
                if (SUCCEEDED(result) && controller)
                    resultController = controller;
                done = true;
                return S_OK;
            }).Get());

    if (FAILED(hr))
    {
        DLSS_Log("[WebViewHost] CreateEmbedded: CreateCoreWebView2Controller cagrisi basarisiz (hr=0x%08X)", hr);
        m_hwnd = nullptr;
        m_embedded = false;
        return false;
    }

    PumpUntil(done);

    if (FAILED(createHr) || !resultController)
    {
        DLSS_Log("[WebViewHost] CreateEmbedded: controller olusturma tamamlanamadi (createHr=0x%08X, done=%d)", createHr, done ? 1 : 0);
        m_hwnd = nullptr;
        m_embedded = false;
        return false;
    }

    DLSS_Log("[WebViewHost] CreateEmbedded: controller basariyla olusturuldu, FinishSetup cagriliyor.");
    FinishSetup(resultController.Get());
    return true;
}

void WebViewHost::Resize(const RECT& /*clientRect*/)
{
    // clientRect parametresi API ile uyum icin tutuluyor; HandleResize zaten
    // m_hwnd'nin GUNCEL client rect'ini okuyor, bu yeterli ve daha az hataya
    // acik (cagiran taraf yanlis/eski bir RECT hazirlarsa sapma olmaz).
    HandleResize();
}

void WebViewHost::FinishSetup(ICoreWebView2Controller* controller)
{
    m_controller = controller;

    RECT rc;
    GetClientRect(m_hwnd, &rc);
    m_controller->put_Bounds(rc);

    // Ilk boyamadan (CSS yuklenene kadar gecen kisa an) once WebView2'nin
    // varsayilan BEYAZ arka plani yerine uygulamanin kendi koyu tema rengini
    // (web/shared/style.css --bg: #0f131a) goster -- beyaz/magenta bir "flash"
    // yerine kesintisiz koyu tema.
    ComPtr<ICoreWebView2Controller2> controller2;
    if (SUCCEEDED(controller->QueryInterface(IID_PPV_ARGS(&controller2))) && controller2)
    {
        COREWEBVIEW2_COLOR darkBg{ 255, 0x00, 0x00, 0x00 };
        controller2->put_DefaultBackgroundColor(darkBg);
    }
    else
    {
        DLSS_Log("[WebViewHost] ICoreWebView2Controller2 alinamadi -- DefaultBackgroundColor tani testi atlandi.");
    }

    ComPtr<ICoreWebView2> webview;
    m_controller->get_CoreWebView2(&webview);
    m_webview = webview;

    if (!m_webview)
    {
        DLSS_Log("[WebViewHost] FinishSetup: get_CoreWebView2 basarisiz oldu, webview NULL.");
        return;
    }

    // TANI AMACLI: renderer/GPU alt surecleri sessizce cokerse (NavigationCompleted
    // hala "success" der, cunku o sadece DOM/ag seviyesini izler) hicbir kare asla
    // ekrana gelmez -- tam bizim gordugumuz "bembeyaz, sonsuza kadar" belirtisi.
    m_webview->add_ProcessFailed(
        Callback<ICoreWebView2ProcessFailedEventHandler>(
            [this](ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs* args) -> HRESULT
            {
                COREWEBVIEW2_PROCESS_FAILED_KIND kind = COREWEBVIEW2_PROCESS_FAILED_KIND_UNKNOWN_PROCESS_EXITED;
                if (args) args->get_ProcessFailedKind(&kind);
                DLSS_Log("[WebViewHost] !!! ProcessFailed (host=%ls): kind=%d !!!",
                    m_virtualHostName.c_str(), static_cast<int>(kind));
                return S_OK;
            }).Get(),
        nullptr);

    // Sag-tik context menu / F12 DevTools uretim derlemesinde kapali kalsin.
    ComPtr<ICoreWebView2Settings> settings;
    if (SUCCEEDED(m_webview->get_Settings(&settings)) && settings)
    {
#ifdef NDEBUG
        settings->put_AreDevToolsEnabled(FALSE);
        settings->put_AreDefaultContextMenusEnabled(FALSE);
#endif
        settings->put_IsStatusBarEnabled(FALSE);
        settings->put_AreDefaultScriptDialogsEnabled(TRUE);
    }

    ComPtr<ICoreWebView2_3> webview3;
    if (SUCCEEDED(m_webview.As(&webview3)) && webview3)
    {
        HRESULT hrMap = webview3->SetVirtualHostNameToFolderMapping(
            m_virtualHostName.c_str(), m_virtualHostFolder.c_str(),
            COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
        DLSS_Log("[WebViewHost] SetVirtualHostNameToFolderMapping('%ls' -> '%ls') hr=0x%08X",
            m_virtualHostName.c_str(), m_virtualHostFolder.c_str(), hrMap);

        // Ortak CSS/JS (web\shared\) her pencereye "https://vlss5.shared/..." olarak
        // haritalanir. virtualHostFolder her zaman "...\web\<pencere>" oldugundan
        // bir ust dizindeki "shared" klasoru hesaplanabilir.
        std::wstring sharedFolder = m_virtualHostFolder;
        size_t slash = sharedFolder.find_last_of(L"\\/");
        if (slash != std::wstring::npos)
        {
            sharedFolder = sharedFolder.substr(0, slash) + L"\\shared";
            HRESULT hrShared = webview3->SetVirtualHostNameToFolderMapping(
                L"vlss5.shared", sharedFolder.c_str(),
                COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
            DLSS_Log("[WebViewHost] SetVirtualHostNameToFolderMapping('vlss5.shared' -> '%ls') hr=0x%08X",
                sharedFolder.c_str(), hrShared);
        }
    }
    else
    {
        DLSS_Log("[WebViewHost] FinishSetup: ICoreWebView2_3 alinamadi -- virtual host mapping YAPILAMADI (WebView2 Runtime cok eski olabilir).");
    }

    // Navigasyonun gercekten basarili olup olmadigini goster -- sessiz beyaz
    // ekran vakalarinda (orn. yanlis virtual host klasoru, dosya bulunamadi)
    // asil hatayi burada goruruz.
    m_webview->add_NavigationCompleted(
        Callback<ICoreWebView2NavigationCompletedEventHandler>(
            [this](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT
            {
                BOOL success = FALSE;
                COREWEBVIEW2_WEB_ERROR_STATUS status = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
                if (args)
                {
                    args->get_IsSuccess(&success);
                    args->get_WebErrorStatus(&status);
                }
                DLSS_Log("[WebViewHost] NavigationCompleted (host=%ls): success=%d errorStatus=%d",
                    m_virtualHostName.c_str(), success ? 1 : 0, static_cast<int>(status));

                // Bilinen WebView2 "ilk boyama" sorunu: compositor bazen
                // put_Bounds ILK cagrildiginda degil, boyut GERCEKTEN
                // DEGISTIGINDE ilk kareyi cizmeye "uyaniyor". Ozellikle
                // surecin OMRUNDE OLUSTURULAN ILK controller'da (bu pencere
                // -- popup'lardan farkli olarak kullanici hicbir sey acmadan
                // hemen olusuyor) bembeyaz/bos kalip hicbir zaman ilk kareyi
                // cizmeyebiliyor. 1px kucult + hemen geri buyut "nudge"'i
                // compositor'i zorlayarak bunu cozen bilinen bir yontem.
                if (success && m_controller && m_hwnd)
                {
                    RECT rc;
                    GetClientRect(m_hwnd, &rc);
                    if (rc.right > rc.left)
                    {
                        RECT rcNudge = rc;
                        rcNudge.right -= 1;
                        m_controller->put_Bounds(rcNudge);
                        m_controller->put_Bounds(rc);
                    }
                }
                return S_OK;
            }).Get(),
        nullptr);

    EventRegistrationToken token{};
    m_webview->add_WebMessageReceived(
        Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [this](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT
            {
                LPWSTR json = nullptr;
                if (SUCCEEDED(args->get_WebMessageAsJson(&json)) && json)
                {
                    // "__drag__": sahte baslik cubugundan gelen surukleme
                    // istegi -- WebView2 "-webkit-app-region: drag" CSS'ini
                    // native pencere surumune baglamadigi icin JS bunu
                    // mousedown'da JSON komutu olarak gonderiyor. Klasik
                    // "borderless pencere surukleme" numarasi: fare yakalamayi
                    // birak, ardindan pencereye sanki baslik cubugundan
                    // tiklanmis gibi WM_NCLBUTTONDOWN/HTCAPTION gonder --
                    // bundan sonrasini OS'un kendi surukleme dongusu yonetir.
                    // Bu, JSON komut yonlendirmesine (m_onMessage) hic girmeden
                    // burada, WebViewHost seviyesinde ele alinir cunku sadece
                    // genel pencere davranisi, pencereye ozgu is mantigi degil.
                    if (m_hwnd && wcsstr(json, L"\"__drag__\""))
                    {
                        ReleaseCapture();
                        SendMessageW(m_hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
                        CoTaskMemFree(json);
                        return S_OK;
                    }

                    // Son savunma hatti: bir istisna bu COM callback sinirini
                    // asarsa std::terminate cagrilip TUM uygulama sessizce
                    // cokebilir (crash dump/log OLUSMAZ). Handler'lar kendi
                    // try/catch'lerini icerse de, burada da yakalamak tek bir
                    // hatali mesajin butun VLSS5'i kapatmasini engeller.
                    try { if (m_onMessage) m_onMessage(json); }
                    catch (...) { OutputDebugStringW(L"[WebViewHost] onMessage istisna firlatti, yoksayildi.\n"); }
                    CoTaskMemFree(json);
                }
                return S_OK;
            }).Get(),
        &token);
    m_msgToken      = token;
    m_msgTokenValid = true;

    std::wstring url = L"https://" + m_virtualHostName + L"/" + m_startPage;
    DLSS_Log("[WebViewHost] Navigate ediliyor: '%ls' (klasor='%ls')", url.c_str(), m_virtualHostFolder.c_str());
    HRESULT hrNav = m_webview->Navigate(url.c_str());
    if (FAILED(hrNav))
        DLSS_Log("[WebViewHost] Navigate cagrisi basarisiz oldu, hr=0x%08X", hrNav);
}

void WebViewHost::Show()
{
    if (!m_hwnd) return;
    ShowWindow(m_hwnd, SW_SHOW);
    SetForegroundWindow(m_hwnd);
    if (m_controller) m_controller->put_IsVisible(TRUE);
}

void WebViewHost::Hide()
{
    if (!m_hwnd) return;
    if (m_controller) m_controller->put_IsVisible(FALSE);
    ShowWindow(m_hwnd, SW_HIDE);
}

bool WebViewHost::IsOpen() const
{
    return m_hwnd && IsWindowVisible(m_hwnd);
}

void WebViewHost::PostJson(const std::wstring& json)
{
    if (m_webview)
        m_webview->PostWebMessageAsJson(json.c_str());
}
