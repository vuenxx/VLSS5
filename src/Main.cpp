#include "Common.h"
#include "App.h"
#include "WindowEnumerator.h"
#include "DLSSManager.h"
#include "ConfigManager.h"
#include "SettingsWindow.h"
#include "resource.h"
#include <shlwapi.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <algorithm>
#include <cwctype>
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")

// ---------------------------------------------------------------------------
// Control IDs
// ---------------------------------------------------------------------------
#define IDC_WINDOWLIST        101
#define IDC_BTN_START         102
#define IDC_BTN_REFRESH       103
#define IDC_LBL_STATUS        104
#define IDC_LBL_KEYBIND       105   // Shows current keybind text
#define IDC_BTN_KEYBIND       106   // "Değiştir" / "İptal"
#define IDC_CHK_VSYNC         107   // VSync Toggle Checkbox
#define IDC_CHK_FPS           108   // FPS Display Checkbox
#define IDC_CHK_DLSS          109   // DLSS 5 Toggle Checkbox
#define IDC_BTN_DLSS_SETTINGS 110   // DLSS 5 Settings Button
#define IDC_COMBO_GPU         111   // GPU Selection ComboBox
#define IDC_BTN_GPU_HELP      112   // GPU Help '?' Button
#define ID_GLOBAL_HOTKEY      201   // Global capture toggle hotkey
#define IDT_HOTKEY_TIMER      301   // Fallback hotkey poller (50ms)

// ---------------------------------------------------------------------------
// Modern Theme Palette (Matches Mockup 1:1)
// ---------------------------------------------------------------------------
static const COLORREF COLOR_BG          = RGB(11, 15, 20);     // Deep midnight charcoal
static const COLORREF COLOR_CARD_BG     = RGB(22, 28, 38);     // Elevated dark surface
static const COLORREF COLOR_BORDER      = RGB(38, 48, 65);     // Subtle surface border
static const COLORREF COLOR_LIME_ACCENT = RGB(162, 238, 56);   // Vibrant lime/neon green matching mockup
static const COLORREF COLOR_LIME_HOVER  = RGB(180, 248, 75);   // Lighter lime hover
static const COLORREF COLOR_LIME_DARK   = RGB(138, 208, 42);   // Pressed lime green
static const COLORREF COLOR_DARK_TEXT   = RGB(12, 18, 10);     // Dark charcoal text on lime
static const COLORREF COLOR_NEON_GREEN  = RGB(0, 240, 55);     // Electric neon green
static const COLORREF COLOR_TEXT_MAIN   = RGB(242, 247, 252);  // Crisp white
static const COLORREF COLOR_TEXT_MUTED  = RGB(139, 148, 158);  // Slate secondary text

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
static std::unique_ptr<App>    g_app;
static std::vector<WindowInfo> g_windows;

struct GpuAdapterInfo
{
    std::wstring name;
    std::wstring displayName;
};
static std::vector<GpuAdapterInfo> g_gpuList;

static HWND                    g_mainHwnd        = nullptr;
static HWND                    g_listBox         = nullptr;
static HWND                    g_lblStatus       = nullptr;
static HWND                    g_lblGpu          = nullptr;
static HWND                    g_comboGpu        = nullptr;
static HWND                    g_btnGpuHelp      = nullptr;
static HWND                    g_tipGpuHelp      = nullptr;
static HWND                    g_lblKeybindTitle = nullptr;
static HWND                    g_lblKeybind      = nullptr;
static HWND                    g_btnKeybind      = nullptr;
static HWND                    g_btnDlssSettings = nullptr;
static HWND                    g_chkVSync        = nullptr;
static HWND                    g_chkFps          = nullptr;
static HWND                    g_chkDlss         = nullptr;
static HWND                    g_btnRefresh      = nullptr;
static HWND                    g_btnStart        = nullptr;

static HFONT                   g_fontNormal   = nullptr;
static HFONT                   g_fontTitle    = nullptr;
static HFONT                   g_fontSubtitle = nullptr;
static HFONT                   g_fontBold     = nullptr;
static HFONT                   g_fontSmall    = nullptr;

static HBRUSH                  g_brBg         = nullptr;
static HBRUSH                  g_brCard       = nullptr;
static HBRUSH                  g_brBorder     = nullptr;
static HBRUSH                  g_brNeon       = nullptr;
static HICON                   g_hLogoHeader  = nullptr;

static bool                    g_vsyncEnabled = false;
static bool                    g_fpsEnabled   = true;
static bool                    g_dlssEnabled  = true;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static void SetStatus(const wchar_t* text)
{
    if (g_lblStatus)
    {
        SetWindowTextW(g_lblStatus, text);
        InvalidateRect(g_lblStatus, nullptr, TRUE);
    }
}

static void UpdateKeybindLabel()
{
    if (g_lblKeybind)
    {
        SetWindowTextW(g_lblKeybind,
            InputForwarder::Config().FormatDisplay().c_str());
        InvalidateRect(g_lblKeybind, nullptr, TRUE);
    }
}

static void PopulateList(HWND menuHwnd)
{
    g_windows = WindowEnumerator::GetWindows(menuHwnd);
    SendMessageW(g_listBox, LB_RESETCONTENT, 0, 0);
    for (auto& w : g_windows)
    {
        SendMessageW(g_listBox, LB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(w.title.c_str()));
    }
    if (!g_windows.empty())
        SendMessageW(g_listBox, LB_SETCURSEL, 0, 0);
}

static bool ContainsCaseInsensitive(const std::wstring& str, const std::wstring& search)
{
    if (search.empty()) return true;
    if (str.length() < search.length()) return false;
    auto it = std::search(
        str.begin(), str.end(),
        search.begin(), search.end(),
        [](wchar_t ch1, wchar_t ch2) {
            return towlower(ch1) == towlower(ch2);
        });
    return (it != str.end());
}

static HWND CreateButtonTooltip(HWND hParent, HWND hTarget, const wchar_t* text)
{
    HWND hTip = CreateWindowExW(
        WS_EX_TOPMOST,
        TOOLTIPS_CLASS,
        nullptr,
        WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP | TTS_BALLOON,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        hParent, nullptr, GetModuleHandleW(nullptr), nullptr);

    if (!hTip) return nullptr;

    SetWindowPos(hTip, HWND_TOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    TOOLINFOW ti = {};
    ti.cbSize   = sizeof(TOOLINFOW);
    ti.uFlags   = TTF_SUBCLASS | TTF_IDISHWND;
    ti.hwnd     = hParent;
    ti.uId      = reinterpret_cast<UINT_PTR>(hTarget);
    ti.lpszText = const_cast<LPWSTR>(text);

    SendMessageW(hTip, TTM_ADDTOOL, 0, reinterpret_cast<LPARAM>(&ti));
    SendMessageW(hTip, TTM_SETMAXTIPWIDTH, 0, 360);
    SendMessageW(hTip, TTM_SETDELAYTIME, TTDT_INITIAL, 50);
    SendMessageW(hTip, TTM_SETDELAYTIME, TTDT_AUTOPOP, 10000);

    return hTip;
}

static void PopulateGpuList(HWND /*hwnd*/)
{
    if (!g_comboGpu) return;

    g_gpuList.clear();
    SendMessageW(g_comboGpu, CB_RESETCONTENT, 0, 0);

    // Option 0: Auto (RTX Priority)
    GpuAdapterInfo autoOpt;
    autoOpt.name = L"Auto";
    autoOpt.displayName = L"⚡ Otomatik (RTX Öncelikli)";
    g_gpuList.push_back(autoOpt);

    ComPtr<IDXGIFactory1> factory;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
    {
        UINT i = 0;
        ComPtr<IDXGIAdapter1> adapter;
        while (factory->EnumAdapters1(i++, &adapter) != DXGI_ERROR_NOT_FOUND)
        {
            DXGI_ADAPTER_DESC1 desc{};
            adapter->GetDesc1(&desc);

            if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
            {
                GpuAdapterInfo info;
                info.name = desc.Description;
                SIZE_T vramMB = desc.DedicatedVideoMemory / (1024 * 1024);

                wchar_t disp[256];
                if (vramMB >= 1024)
                {
                    swprintf_s(disp, L"%ls (%.1f GB)", desc.Description, static_cast<float>(vramMB) / 1024.0f);
                }
                else if (vramMB > 0)
                {
                    swprintf_s(disp, L"%ls (%zu MB)", desc.Description, vramMB);
                }
                else
                {
                    swprintf_s(disp, L"%ls", desc.Description);
                }

                info.displayName = disp;
                g_gpuList.push_back(info);
            }
        }
    }

    if (g_gpuList.size() <= 1)
    {
        GpuAdapterInfo info;
        info.name = L"Varsayılan Grafik Kartı";
        info.displayName = L"Varsayılan Grafik Kartı";
        g_gpuList.push_back(info);
    }

    const std::wstring& savedGpu = ConfigManager::Get().Config().selectedGpu;
    int selectedIndex = 0; // Default to "⚡ Otomatik (RTX Öncelikli)"

    if (savedGpu.empty() || savedGpu == L"Auto")
    {
        selectedIndex = 0;
        // Check if an RTX card is available to log/verify
        bool hasRtx = false;
        for (size_t idx = 1; idx < g_gpuList.size(); ++idx)
        {
            if (ContainsCaseInsensitive(g_gpuList[idx].name, L"RTX"))
            {
                hasRtx = true;
                DLSS_Log("[Main] GPU Auto mode: RTX adapter ready (%ls)", g_gpuList[idx].name.c_str());
                break;
            }
        }
        if (!hasRtx && g_gpuList.size() > 1)
        {
            SetStatus(L"⚠ Sistemde RTX kart bulunamadı! DLSS5 için RTX gereklidir.");
        }

        if (savedGpu.empty())
        {
            ConfigManager::Get().Config().selectedGpu = L"Auto";
            ConfigManager::Get().Save();
        }
    }
    else
    {
        // Check if the saved GPU exists on this machine
        int matchedIdx = -1;
        for (size_t idx = 1; idx < g_gpuList.size(); ++idx)
        {
            if (ContainsCaseInsensitive(g_gpuList[idx].name, savedGpu))
            {
                matchedIdx = static_cast<int>(idx);
                break;
            }
        }

        if (matchedIdx != -1)
        {
            selectedIndex = matchedIdx;
        }
        else
        {
            // SELF-HEALING: The saved config references hardware not present on this machine!
            // Automatically find the best GPU on this system (prioritize RTX).
            int rtxIdx = -1;
            for (size_t idx = 1; idx < g_gpuList.size(); ++idx)
            {
                if (ContainsCaseInsensitive(g_gpuList[idx].name, L"RTX"))
                {
                    rtxIdx = static_cast<int>(idx);
                    break;
                }
            }

            if (rtxIdx != -1)
            {
                selectedIndex = rtxIdx;
                ConfigManager::Get().Config().selectedGpu = g_gpuList[rtxIdx].name;
                ConfigManager::Get().Save();

                std::wstring msg = L"Farklı donanım tespit edildi. Sisteminizdeki " + g_gpuList[rtxIdx].name + L" otomatik seçildi.";
                SetStatus(msg.c_str());
                DLSS_Log("[Main] Foreign GPU '%ls' not found. Self-healed config to: %ls", savedGpu.c_str(), g_gpuList[rtxIdx].name.c_str());
            }
            else
            {
                selectedIndex = 0; // Fallback to Auto
                ConfigManager::Get().Config().selectedGpu = L"Auto";
                ConfigManager::Get().Save();

                SetStatus(L"⚠ Farklı donanım tespit edildi ve RTX kart bulunamadı! (DLSS5 için RTX gereklidir)");
                DLSS_Log("[Main] Foreign GPU '%ls' not found. No RTX detected on this system.", savedGpu.c_str());
            }
        }
    }

    for (size_t idx = 0; idx < g_gpuList.size(); ++idx)
    {
        SendMessageW(g_comboGpu, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(g_gpuList[idx].displayName.c_str()));
    }

    SendMessageW(g_comboGpu, CB_SETCURSEL, selectedIndex, 0);
}

// ---------------------------------------------------------------------------
// Custom UI Controls Drawing
// ---------------------------------------------------------------------------
static void DrawModernPanel(HDC hdc, const RECT& rc, COLORREF bg, COLORREF border, int radius = 8)
{
    HPEN hPen = CreatePen(PS_SOLID, 1, border);
    HBRUSH hBr = CreateSolidBrush(bg);
    HGDIOBJ oldPen = SelectObject(hdc, hPen);
    HGDIOBJ oldBr = SelectObject(hdc, hBr);

    RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, radius, radius);

    SelectObject(hdc, oldBr);
    SelectObject(hdc, oldPen);
    DeleteObject(hBr);
    DeleteObject(hPen);
}

// ---------------------------------------------------------------------------
// Hotkey & Capture Management
// ---------------------------------------------------------------------------
static DWORD g_lastHotkeyTick = 0;

static void RegisterAppHotkey(HWND hwnd)
{
    UnregisterHotKey(hwnd, ID_GLOBAL_HOTKEY);
    const HotkeyConfig& cfg = InputForwarder::Config();
    RegisterHotKey(hwnd, ID_GLOBAL_HOTKEY, cfg.modifiers, cfg.vk);
}

static void UnregisterAppHotkey(HWND hwnd)
{
    UnregisterHotKey(hwnd, ID_GLOBAL_HOTKEY);
}

static bool StartCaptureWithTarget(HWND hwnd, HWND target)
{
    if (!IsWindow(target))
    {
        SetStatus(L"Seçilen pencere artık açık değil — Yenile'ye basın.");
        return false;
    }

    ShowWindow(hwnd, SW_HIDE);

    if (!g_app->StartOverlay(hwnd, target, g_vsyncEnabled, g_dlssEnabled, g_fpsEnabled))
    {
        ShowWindow(hwnd, SW_SHOW);
        SetForegroundWindow(hwnd);
        SetStatus(L"Overlay başlatılamadı. Pencereyi kontrol edin.");
        return false;
    }

    // Blocking render loop
    g_app->Run();

    // Prevent immediate hotkey re-trigger when returning from overlay
    g_lastHotkeyTick = GetTickCount();
    MSG flushMsg = {};
    while (PeekMessageW(&flushMsg, nullptr, WM_HOTKEY, WM_HOTKEY, PM_REMOVE)) {}

    // Wait until key is released (up to 300ms) so releasing Alt+S doesn't trigger start
    DWORD waitStart = GetTickCount();
    while (InputForwarder::IsStopKeyDown() && (GetTickCount() - waitStart < 300))
    {
        Sleep(10);
    }

    // When returned, restore main window
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    BringWindowToTop(hwnd);
    PopulateList(hwnd);

    std::wstring msg = L"Overlay durduruldu.  [";
    msg += InputForwarder::Config().FormatDisplay();
    msg += L"] veya BAŞLAT ile tekrar açabilirsiniz.";
    SetStatus(msg.c_str());
    return true;
}

static void TriggerCaptureToggle(HWND hwnd)
{
    DWORD now = GetTickCount();
    if (now - g_lastHotkeyTick < 400) // 400ms debounce
        return;
    g_lastHotkeyTick = now;

    if (g_app && g_app->GetState() == AppState::Capturing)
    {
        // Currently capturing -> Stop it!
        g_app->RequestStop();
        return;
    }

    // Currently in Menu -> Start it!
    HWND target = nullptr;
    HWND fg = GetForegroundWindow();

    // 1. Check if the foreground window is an active application/game window (not our menu, not shell)
    if (fg && fg != hwnd && IsWindow(fg) && IsWindowVisible(fg))
    {
        wchar_t cls[64] = {};
        GetClassNameW(fg, cls, _countof(cls));
        if (wcscmp(cls, L"Progman") != 0 &&
            wcscmp(cls, L"WorkerW") != 0 &&
            wcscmp(cls, L"Shell_TrayWnd") != 0 &&
            wcscmp(cls, L"Shell_SecondaryTrayWnd") != 0)
        {
            target = fg;
        }
    }

    // 2. If foreground was the menu window itself or a system window, use the selected window in the list
    if (!target)
    {
        int sel = static_cast<int>(SendMessageW(g_listBox, LB_GETCURSEL, 0, 0));
        if (sel != LB_ERR && sel < static_cast<int>(g_windows.size()))
        {
            target = g_windows[sel].hwnd;
        }
        else if (!g_windows.empty())
        {
            target = g_windows[0].hwnd;
        }
    }

    if (target && IsWindow(target))
    {
        // Highlight in list if present
        for (size_t i = 0; i < g_windows.size(); ++i)
        {
            if (g_windows[i].hwnd == target)
            {
                SendMessageW(g_listBox, LB_SETCURSEL, static_cast<WPARAM>(i), 0);
                break;
            }
        }

        StartCaptureWithTarget(hwnd, target);
    }
    else
    {
        SetStatus(L"Yakalanacak pencere bulunamadı. Lütfen listeden seçin.");
    }
}

// ---------------------------------------------------------------------------
// WndProc
// ---------------------------------------------------------------------------
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        g_mainHwnd = hwnd;

        // Create fonts with Segoe UI for crisp rendering & Turkish character support
        g_fontTitle = CreateFontW(-22, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

        g_fontSubtitle = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

        g_fontNormal = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

        g_fontBold = CreateFontW(-14, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

        g_fontSmall = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

        // Theme brushes
        g_brBg     = CreateSolidBrush(COLOR_BG);
        g_brCard   = CreateSolidBrush(COLOR_CARD_BG);
        g_brBorder = CreateSolidBrush(COLOR_BORDER);
        g_brNeon   = CreateSolidBrush(COLOR_NEON_GREEN);

        // Load 46x46 logo icon for header bar
        HINSTANCE hInst = reinterpret_cast<LPCREATESTRUCTW>(lParam)->hInstance;
        g_hLogoHeader = reinterpret_cast<HICON>(LoadImageW(
            hInst, MAKEINTRESOURCEW(IDI_MAIN_ICON), IMAGE_ICON, 46, 46, LR_DEFAULTCOLOR));

        auto SF = [](HWND h, HFONT f) {
            SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(f), TRUE);
        };

        // ---- Window List Section (Card 1: Hedef Uygulama Seçimi) ----
        // Left Column: ListBox for target windows
        g_listBox = CreateWindowExW(0, L"LISTBOX", nullptr,
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT,
            34, 112, 362, 178,
            hwnd, reinterpret_cast<HMENU>(IDC_WINDOWLIST), nullptr, nullptr);
        SF(g_listBox, g_fontNormal);
        SetWindowTheme(g_listBox, L"DarkMode_Explorer", nullptr);
        SendMessageW(g_listBox, LB_SETITEMHEIGHT, 0, 36);

        // Right Column (Inside Card 1)
        // Refresh Button
        g_btnRefresh = CreateWindowW(L"BUTTON", L"Yenile    ↻",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            412, 112, 158, 34,
            hwnd, reinterpret_cast<HMENU>(IDC_BTN_REFRESH), nullptr, nullptr);
        SF(g_btnRefresh, g_fontBold);

        // VSync Checkbox
        g_chkVSync = CreateWindowW(L"BUTTON", L"VSync",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            412, 158, 158, 26,
            hwnd, reinterpret_cast<HMENU>(IDC_CHK_VSYNC), nullptr, nullptr);
        SF(g_chkVSync, g_fontBold);

        // FPS Overlay Checkbox
        g_chkFps = CreateWindowW(L"BUTTON", L"FPS Göstergesi",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            412, 194, 158, 26,
            hwnd, reinterpret_cast<HMENU>(IDC_CHK_FPS), nullptr, nullptr);
        SF(g_chkFps, g_fontBold);

        // VLSS5 Checkbox
        g_chkDlss = CreateWindowW(L"BUTTON", L"VLSS5 (Nöral)",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            412, 230, 158, 26,
            hwnd, reinterpret_cast<HMENU>(IDC_CHK_DLSS), nullptr, nullptr);
        SF(g_chkDlss, g_fontBold);

        // ---- Card 2: GPU & Kısayol Paneli ----
        // Row 1: GPU Selection
        g_lblGpu = CreateWindowW(L"STATIC", L"Grafik Kartı (GPU):",
            WS_CHILD | WS_VISIBLE, 32, 328, 130, 20,
            hwnd, nullptr, nullptr, nullptr);
        SF(g_lblGpu, g_fontBold);

        g_comboGpu = CreateWindowExW(0, L"COMBOBOX", nullptr,
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWNLIST,
            168, 324, 370, 200,
            hwnd, reinterpret_cast<HMENU>(IDC_COMBO_GPU), nullptr, nullptr);
        SF(g_comboGpu, g_fontNormal);
        SetWindowTheme(g_comboGpu, L"DarkMode_Explorer", nullptr);

        g_btnGpuHelp = CreateWindowW(L"BUTTON", L"?",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            544, 323, 26, 26,
            hwnd, reinterpret_cast<HMENU>(IDC_BTN_GPU_HELP), nullptr, nullptr);
        SF(g_btnGpuHelp, g_fontBold);

        g_tipGpuHelp = CreateButtonTooltip(hwnd, g_btnGpuHelp,
            L"DLSS5 kullanmak için RTX bir kart gereklidir. AMD kartlarda çalışmaz!");

        // Row 2: Overlay Shortcut & VLSS5 Settings
        g_lblKeybindTitle = CreateWindowW(L"STATIC", L"Overlay Kısayolu:",
            WS_CHILD | WS_VISIBLE, 32, 368, 130, 20,
            hwnd, nullptr, nullptr, nullptr);
        SF(g_lblKeybindTitle, g_fontBold);

        g_lblKeybind = CreateWindowW(L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | SS_OWNERDRAW,
            168, 364, 96, 30,
            hwnd, reinterpret_cast<HMENU>(IDC_LBL_KEYBIND), nullptr, nullptr);
        SF(g_lblKeybind, g_fontBold);
        UpdateKeybindLabel();

        g_btnKeybind = CreateWindowW(L"BUTTON", L"⌨ Değiştir",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            272, 364, 92, 30,
            hwnd, reinterpret_cast<HMENU>(IDC_BTN_KEYBIND), nullptr, nullptr);
        SF(g_btnKeybind, g_fontNormal);

        g_btnDlssSettings = CreateWindowW(L"BUTTON", L"⚙ VLSS5 Ayarları",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            372, 364, 198, 30,
            hwnd, reinterpret_cast<HMENU>(IDC_BTN_DLSS_SETTINGS), nullptr, nullptr);
        SF(g_btnDlssSettings, g_fontBold);

        // ---- Start Button (Big Action Button) ----
        g_btnStart = CreateWindowW(L"BUTTON", L"BAŞLAT  ➔",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            20, 420, 566, 48,
            hwnd, reinterpret_cast<HMENU>(IDC_BTN_START), nullptr, nullptr);
        SF(g_btnStart, g_fontTitle);

        // ---- Status bar ----
        g_lblStatus = CreateWindowW(L"STATIC", L"Hedef uygulamayı seçin veya istediğiniz penceredeyken ALT+S basın.",
            WS_CHILD | WS_VISIBLE | SS_CENTER, 20, 478, 566, 20,
            hwnd, reinterpret_cast<HMENU>(IDC_LBL_STATUS), nullptr, nullptr);
        SF(g_lblStatus, g_fontSmall);

        // Global hotkey registration & fallback timer
        RegisterAppHotkey(hwnd);
        SetTimer(hwnd, IDT_HOTKEY_TIMER, 50, nullptr);

        // Populate initial lists
        PopulateList(hwnd);
        PopulateGpuList(hwnd);
        break;
    }

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        RECT client;
        GetClientRect(hwnd, &client);

        // 1. Fill entire window background (Dark Midnight)
        FillRect(hdc, &client, g_brBg);

        // 2. Draw Top Header Bar
        RECT headerRect = { 0, 0, client.right, 66 };
        FillRect(hdc, &headerRect, g_brCard);

        // Header bottom accent line (Glowing Lime Line)
        HPEN hPenGlow = CreatePen(PS_SOLID, 3, RGB(0, 90, 25));
        HGDIOBJ oldPen = SelectObject(hdc, hPenGlow);
        MoveToEx(hdc, 0, 65, nullptr);
        LineTo(hdc, client.right, 65);

        HPEN hPenNeon = CreatePen(PS_SOLID, 1, COLOR_LIME_ACCENT);
        SelectObject(hdc, hPenNeon);
        MoveToEx(hdc, 0, 65, nullptr);
        LineTo(hdc, client.right, 65);
        SelectObject(hdc, oldPen);
        DeleteObject(hPenNeon);
        DeleteObject(hPenGlow);

        // Header Emblem / Logo (42x42)
        if (g_hLogoHeader)
        {
            DrawIconEx(hdc, 20, 12, g_hLogoHeader, 42, 42, 0, nullptr, DI_NORMAL);
        }

        // Header Title (Bold White)
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(255, 255, 255));
        SelectObject(hdc, g_fontTitle);
        TextOutW(hdc, 74, 12, L"VLSS5", 5);

        // Header Subtitle
        SetTextColor(hdc, COLOR_TEXT_MUTED);
        SelectObject(hdc, g_fontSubtitle);
        const wchar_t subTitle[] = L"Youtube: @vuenxxmx";
        TextOutW(hdc, 74, 38, subTitle, static_cast<int>(wcslen(subTitle)));

        // Keyboard Tips in Header (Right-side Pill Container)
        RECT rcTips = { client.right - 340, 18, client.right - 20, 48 };
        DrawModernPanel(hdc, rcTips, RGB(14, 18, 25), COLOR_BORDER, 6);

        SetTextColor(hdc, RGB(165, 175, 190));
        SelectObject(hdc, g_fontSmall);
        const wchar_t tips[] = L"[F8] Odak  |  [F9] FPS  |  [F10] VLSS5  |  [Alt+S] Başlat";
        DrawTextW(hdc, tips, -1, &rcTips, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        // 3. Card 1 Panel (Hedef Uygulama Seçimi)
        RECT card1 = { 20, 76, client.right - 20, 304 };
        DrawModernPanel(hdc, card1, COLOR_CARD_BG, COLOR_BORDER, 10);

        // Card 1 Title
        SetTextColor(hdc, RGB(225, 232, 242));
        SelectObject(hdc, g_fontBold);
        TextOutW(hdc, 34, 88, L"HEDEF UYGULAMA SEÇİMİ", 21);

        // 4. Card 2 Panel (GPU & Kısayol)
        RECT card2 = { 20, 314, client.right - 20, 410 };
        DrawModernPanel(hdc, card2, COLOR_CARD_BG, COLOR_BORDER, 10);

        EndPaint(hwnd, &ps);
        break;
    }

    case WM_DRAWITEM:
    {
        auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
        if (!dis) break;

        // 1. Custom draw the Window ListBox (Items with App Icons & Lime Pill Selection)
        if (dis->CtlID == IDC_WINDOWLIST)
        {
            if (dis->itemID == static_cast<UINT>(-1)) break;

            bool isSelected = (dis->itemState & ODS_SELECTED);

            RECT rc = dis->rcItem;
            int itemW = rc.right - rc.left;
            int itemH = rc.bottom - rc.top;

            HDC memDC = CreateCompatibleDC(dis->hDC);
            HBITMAP memBmp = CreateCompatibleBitmap(dis->hDC, itemW, itemH);
            HGDIOBJ oldBmp = SelectObject(memDC, memBmp);

            // Fill background with card background
            RECT rcLocal = { 0, 0, itemW, itemH };
            FillRect(memDC, &rcLocal, g_brCard);

            if (isSelected)
            {
                // Rounded lime green pill matching mockup
                RECT rcPill = { 2, 2, itemW - 2, itemH - 2 };
                HPEN hPenPill = CreatePen(PS_SOLID, 1, COLOR_LIME_ACCENT);
                HBRUSH hBrPill = CreateSolidBrush(COLOR_LIME_ACCENT);
                HGDIOBJ oldPen = SelectObject(memDC, hPenPill);
                HGDIOBJ oldBr  = SelectObject(memDC, hBrPill);

                RoundRect(memDC, rcPill.left, rcPill.top, rcPill.right, rcPill.bottom, 8, 8);

                SelectObject(memDC, oldBr);
                SelectObject(memDC, oldPen);
                DeleteObject(hBrPill);
                DeleteObject(hPenPill);
            }

            // Get item icon and title
            HICON hIcon = nullptr;
            std::wstring title;
            if (dis->itemID < g_windows.size())
            {
                hIcon = g_windows[dis->itemID].icon;
                title = g_windows[dis->itemID].title;
            }
            else
            {
                wchar_t buf[512] = {};
                SendMessageW(dis->hwndItem, LB_GETTEXT, dis->itemID, reinterpret_cast<LPARAM>(buf));
                title = buf;
            }

            // Draw application icon (20x20)
            int iconSize = 20;
            int iconX = 8;
            int iconY = (itemH - iconSize) / 2;
            if (hIcon)
            {
                DrawIconEx(memDC, iconX, iconY, hIcon, iconSize, iconSize, 0, nullptr, DI_NORMAL);
            }
            else
            {
                HICON defIcon = LoadIconW(nullptr, IDI_APPLICATION);
                if (defIcon)
                    DrawIconEx(memDC, iconX, iconY, defIcon, iconSize, iconSize, 0, nullptr, DI_NORMAL);
            }

            // Draw window title text
            SetBkMode(memDC, TRANSPARENT);
            SetTextColor(memDC, isSelected ? COLOR_DARK_TEXT : COLOR_TEXT_MAIN);
            SelectObject(memDC, isSelected ? g_fontBold : g_fontNormal);

            RECT rcText = { iconX + iconSize + 8, 0, itemW - 8, itemH };
            DrawTextW(memDC, title.c_str(), -1, &rcText,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

            BitBlt(dis->hDC, rc.left, rc.top, itemW, itemH, memDC, 0, 0, SRCCOPY);

            SelectObject(memDC, oldBmp);
            DeleteObject(memBmp);
            DeleteDC(memDC);

            return TRUE;
        }

        // 2. Custom draw Checkboxes (VSync, FPS, DLSS 5) with modern square + checkmark
        if (dis->CtlID == IDC_CHK_VSYNC || dis->CtlID == IDC_CHK_FPS || dis->CtlID == IDC_CHK_DLSS)
        {
            bool isChecked = false;
            const wchar_t* label = L"";
            if (dis->CtlID == IDC_CHK_VSYNC)      { isChecked = g_vsyncEnabled; label = L"VSync"; }
            else if (dis->CtlID == IDC_CHK_FPS)   { isChecked = g_fpsEnabled;   label = L"FPS Göstergesi"; }
            else if (dis->CtlID == IDC_CHK_DLSS)  { isChecked = g_dlssEnabled;  label = L"VLSS5 (Nöral)"; }

            RECT rc = dis->rcItem;
            int itemW = rc.right - rc.left;
            int itemH = rc.bottom - rc.top;

            HDC memDC = CreateCompatibleDC(dis->hDC);
            HBITMAP memBmp = CreateCompatibleBitmap(dis->hDC, itemW, itemH);
            HGDIOBJ oldBmp = SelectObject(memDC, memBmp);

            RECT rcLocal = { 0, 0, itemW, itemH };
            FillRect(memDC, &rcLocal, g_brCard);

            // Checkbox square (18x18)
            int boxSize = 18;
            int boxX = 2;
            int boxY = (itemH - boxSize) / 2;
            RECT rcBox = { boxX, boxY, boxX + boxSize, boxY + boxSize };

            if (isChecked)
            {
                // Dark box with crisp white checkmark matching mockup
                HPEN hPenBox = CreatePen(PS_SOLID, 1, RGB(70, 85, 110));
                HBRUSH hBrBox = CreateSolidBrush(RGB(28, 36, 48));
                HGDIOBJ oldPen = SelectObject(memDC, hPenBox);
                HGDIOBJ oldBr  = SelectObject(memDC, hBrBox);

                RoundRect(memDC, rcBox.left, rcBox.top, rcBox.right, rcBox.bottom, 4, 4);

                // Draw white checkmark ✓
                HPEN hPenCheck = CreatePen(PS_SOLID, 2, RGB(245, 250, 255));
                SelectObject(memDC, hPenCheck);

                MoveToEx(memDC, boxX + 4, boxY + 9, nullptr);
                LineTo(memDC, boxX + 7, boxY + 13);
                LineTo(memDC, boxX + 14, boxY + 5);

                SelectObject(memDC, oldBr);
                SelectObject(memDC, oldPen);
                DeleteObject(hPenCheck);
                DeleteObject(hBrBox);
                DeleteObject(hPenBox);
            }
            else
            {
                // Unchecked: dark box with subtle border
                HPEN hPenBox = CreatePen(PS_SOLID, 1, RGB(55, 68, 88));
                HBRUSH hBrBox = CreateSolidBrush(RGB(20, 26, 36));
                HGDIOBJ oldPen = SelectObject(memDC, hPenBox);
                HGDIOBJ oldBr  = SelectObject(memDC, hBrBox);

                RoundRect(memDC, rcBox.left, rcBox.top, rcBox.right, rcBox.bottom, 4, 4);

                SelectObject(memDC, oldBr);
                SelectObject(memDC, oldPen);
                DeleteObject(hBrBox);
                DeleteObject(hPenBox);
            }

            // Draw label
            SetBkMode(memDC, TRANSPARENT);
            SetTextColor(memDC, COLOR_TEXT_MAIN);
            SelectObject(memDC, g_fontBold);

            RECT rcText = { boxX + boxSize + 10, 0, itemW, itemH };
            DrawTextW(memDC, label, -1, &rcText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

            BitBlt(dis->hDC, rc.left, rc.top, itemW, itemH, memDC, 0, 0, SRCCOPY);

            SelectObject(memDC, oldBmp);
            DeleteObject(memBmp);
            DeleteDC(memDC);

            return TRUE;
        }

        // 3. Custom draw Start Button (Big Lime Action Button: BAŞLAT ➔)
        if (dis->CtlID == IDC_BTN_START)
        {
            bool isPressed = (dis->itemState & ODS_SELECTED);
            COLORREF btnBg = isPressed ? COLOR_LIME_DARK : COLOR_LIME_ACCENT;

            DrawModernPanel(dis->hDC, dis->rcItem, btnBg, btnBg, 10);

            SetBkMode(dis->hDC, TRANSPARENT);
            SetTextColor(dis->hDC, COLOR_DARK_TEXT);
            SelectObject(dis->hDC, g_fontTitle);

            DrawTextW(dis->hDC, L"BAŞLAT  ➔", -1, &dis->rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            return TRUE;
        }

        // 4. Custom draw VLSS5 Settings Button (Vibrant Lime Green in Card 2)
        if (dis->CtlID == IDC_BTN_DLSS_SETTINGS)
        {
            bool isPressed = (dis->itemState & ODS_SELECTED);
            COLORREF btnBg = isPressed ? COLOR_LIME_DARK : COLOR_LIME_ACCENT;

            DrawModernPanel(dis->hDC, dis->rcItem, btnBg, btnBg, 6);

            SetBkMode(dis->hDC, TRANSPARENT);
            SetTextColor(dis->hDC, COLOR_DARK_TEXT);
            SelectObject(dis->hDC, g_fontBold);

            DrawTextW(dis->hDC, L"⚙ VLSS5 Ayarları", -1, &dis->rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            return TRUE;
        }

        // 5. Custom draw Shortcut Pill Badge (Alt+S)
        if (dis->CtlID == IDC_LBL_KEYBIND)
        {
            DrawModernPanel(dis->hDC, dis->rcItem, RGB(14, 18, 25), RGB(45, 58, 78), 6);

            wchar_t keyText[64] = {};
            GetWindowTextW(dis->hwndItem, keyText, _countof(keyText));

            SetBkMode(dis->hDC, TRANSPARENT);
            SetTextColor(dis->hDC, COLOR_TEXT_MAIN);
            SelectObject(dis->hDC, g_fontBold);

            DrawTextW(dis->hDC, keyText, -1, &dis->rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            return TRUE;
        }

        // 6. Custom draw Secondary Buttons (Yenile, Değiştir)
        if (dis->CtlID == IDC_BTN_REFRESH || dis->CtlID == IDC_BTN_KEYBIND)
        {
            bool isPressed = (dis->itemState & ODS_SELECTED);
            COLORREF btnBg = isPressed ? RGB(36, 46, 62) : RGB(26, 34, 46);
            COLORREF btnBorder = isPressed ? COLOR_LIME_ACCENT : RGB(48, 62, 82);

            DrawModernPanel(dis->hDC, dis->rcItem, btnBg, btnBorder, 6);

            wchar_t btnText[64] = {};
            GetWindowTextW(dis->hwndItem, btnText, _countof(btnText));

            SetBkMode(dis->hDC, TRANSPARENT);
            SetTextColor(dis->hDC, COLOR_TEXT_MAIN);
            SelectObject(dis->hDC, g_fontBold);

            DrawTextW(dis->hDC, btnText, -1, &dis->rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            return TRUE;
        }

        // 7. Custom draw GPU Help '?' Button
        if (dis->CtlID == IDC_BTN_GPU_HELP)
        {
            bool isPressed = (dis->itemState & ODS_SELECTED);
            COLORREF btnBg = isPressed ? RGB(36, 46, 62) : RGB(26, 34, 46);
            COLORREF btnBorder = isPressed ? COLOR_LIME_ACCENT : RGB(48, 62, 82);
            COLORREF textColor = isPressed ? COLOR_LIME_ACCENT : COLOR_TEXT_MUTED;

            DrawModernPanel(dis->hDC, dis->rcItem, btnBg, btnBorder, 6);

            SetBkMode(dis->hDC, TRANSPARENT);
            SetTextColor(dis->hDC, textColor);
            SelectObject(dis->hDC, g_fontBold);

            DrawTextW(dis->hDC, L"?", -1, &dis->rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            return TRUE;
        }
        break;
    }

    case WM_CTLCOLORSTATIC:
    {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        HWND ctlHwnd = reinterpret_cast<HWND>(lParam);

        SetBkMode(hdc, TRANSPARENT);

        if (ctlHwnd == g_lblStatus)
        {
            SetTextColor(hdc, COLOR_TEXT_MUTED);
            return reinterpret_cast<LRESULT>(g_brBg);
        }

        SetTextColor(hdc, COLOR_TEXT_MAIN);
        return reinterpret_cast<LRESULT>(g_brCard);
    }

    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLOREDIT:
    {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetBkMode(hdc, OPAQUE);
        SetBkColor(hdc, COLOR_CARD_BG);
        SetTextColor(hdc, COLOR_TEXT_MAIN);
        return reinterpret_cast<LRESULT>(g_brCard);
    }

    case WM_COMMAND:
    {
        int ctlId = LOWORD(wParam);

        // VSync checkbox toggle
        if (ctlId == IDC_CHK_VSYNC)
        {
            g_vsyncEnabled = !g_vsyncEnabled;
            InvalidateRect(g_chkVSync, nullptr, TRUE);
            if (g_vsyncEnabled)
                SetStatus(L"VSync etkin: Kareler monitör yenileme hızına kilitlenecek.");
            else
                SetStatus(L"VSync kapalı: Sınırsız kare hızı & en düşük gecikme.");
            break;
        }

        // FPS checkbox toggle
        if (ctlId == IDC_CHK_FPS)
        {
            g_fpsEnabled = !g_fpsEnabled;
            InvalidateRect(g_chkFps, nullptr, TRUE);
            break;
        }

        // DLSS 5 checkbox toggle
        if (ctlId == IDC_CHK_DLSS)
        {
            g_dlssEnabled = !g_dlssEnabled;
            InvalidateRect(g_chkDlss, nullptr, TRUE);
            if (g_dlssEnabled)
                SetStatus(L"VLSS5 etkin: Nöral iyileştirme devrede.");
            else
                SetStatus(L"VLSS5 kapalı: Standart doğrudan görüntü modu.");
            break;
        }

        // Refresh window & GPU lists
        if (ctlId == IDC_BTN_REFRESH)
        {
            PopulateList(hwnd);
            PopulateGpuList(hwnd);
            SetStatus(L"Pencere ve GPU listesi güncellendi.");
            break;
        }

        // GPU Selection change
        if (ctlId == IDC_COMBO_GPU && HIWORD(wParam) == CBN_SELCHANGE)
        {
            int sel = static_cast<int>(SendMessageW(g_comboGpu, CB_GETCURSEL, 0, 0));
            if (sel >= 0 && sel < static_cast<int>(g_gpuList.size()))
            {
                const std::wstring& chosenName = g_gpuList[sel].name;
                ConfigManager::Get().Config().selectedGpu = chosenName;
                ConfigManager::Get().Save();

                if (g_app)
                {
                    g_app->SetPreferredGpu(chosenName);
                }

                std::wstring statusMsg;
                if (chosenName == L"Auto")
                {
                    statusMsg = L"Grafik kartı: Otomatik (RTX Öncelikli)";
                }
                else
                {
                    statusMsg = L"Grafik kartı seçildi: " + g_gpuList[sel].displayName;
                }
                SetStatus(statusMsg.c_str());
                DLSS_Log("[Main] GPU selection changed to: %ls", chosenName.c_str());
            }
            break;
        }

        // GPU Help '?' button click
        if (ctlId == IDC_BTN_GPU_HELP)
        {
            MessageBoxW(
                hwnd,
                L"DLSS5 kullanmak için RTX bir kart gereklidir. AMD kartlarda çalışmaz!",
                L"VLSS5 — GPU Gereksinimi",
                MB_ICONINFORMATION | MB_OK);
            break;
        }

        // Start overlay
        if (ctlId == IDC_BTN_START)
        {
            int sel = static_cast<int>(SendMessageW(g_listBox, LB_GETCURSEL, 0, 0));
            if (sel == LB_ERR || sel >= static_cast<int>(g_windows.size()))
            {
                SetStatus(L"Lütfen listeden bir pencere seçin.");
                break;
            }

            HWND target = g_windows[sel].hwnd;
            StartCaptureWithTarget(hwnd, target);
            break;
        }

        // Keybind capture
        if (ctlId == IDC_BTN_KEYBIND)
        {
            if (!InputForwarder::IsCapturing())
            {
                UnregisterAppHotkey(hwnd);
                SetWindowTextW(g_lblKeybind, L"Tuşa basın…");
                SetWindowTextW(g_btnKeybind, L"İptal");
                SetStatus(L"Yeni kısayol için bir tuş kombinasyonu basın.");

                InputForwarder::BeginCapture([hwnd](HotkeyConfig /*cfg*/) {
                    PostMessageW(hwnd, WM_APP, 0, 0);
                });
            }
            else
            {
                InputForwarder::EndCapture();
                RegisterAppHotkey(hwnd);
                UpdateKeybindLabel();
                SetWindowTextW(g_btnKeybind, L"Değiştir");
                SetStatus(L"Kısayol değiştirme iptal edildi.");
            }
            break;
        }

        // DLSS 5 Settings window toggle
        if (ctlId == IDC_BTN_DLSS_SETTINGS)
        {
            SettingsWindow::Toggle(hwnd);
            break;
        }
        break;
    }

    case WM_APP:
    {
        RegisterAppHotkey(hwnd);
        UpdateKeybindLabel();
        SetWindowTextW(g_btnKeybind, L"Değiştir");

        const HotkeyConfig& cfg = InputForwarder::Config();
        std::wstring msg = L"Kısayol kaydedildi: [";
        msg += cfg.FormatDisplay();
        msg += L"]";

        if (!cfg.HasRealModifier())
        {
            msg += L"  ⚠ Modifier (Alt/Ctrl/Shift) olmadan atanmış. ";
            msg += L"Çakışmayı önlemek için Alt+" + cfg.FormatDisplay() + L" önerilir.";
        }

        SetStatus(msg.c_str());
        break;
    }

    case WM_HOTKEY:
        if (wParam == ID_GLOBAL_HOTKEY)
        {
            TriggerCaptureToggle(hwnd);
            return 0;
        }
        break;

    case WM_TIMER:
        if (wParam == IDT_HOTKEY_TIMER)
        {
            if (!InputForwarder::IsCapturing())
            {
                if (InputForwarder::IsStopKeyDown())
                {
                    TriggerCaptureToggle(hwnd);
                }
            }
            return 0;
        }
        break;

    case WM_DESTROY:
        KillTimer(hwnd, IDT_HOTKEY_TIMER);
        UnregisterAppHotkey(hwnd);
        InputForwarder::EndCapture();
        if (g_fontNormal)   { DeleteObject(g_fontNormal);   g_fontNormal   = nullptr; }
        if (g_fontTitle)    { DeleteObject(g_fontTitle);    g_fontTitle    = nullptr; }
        if (g_fontSubtitle) { DeleteObject(g_fontSubtitle); g_fontSubtitle = nullptr; }
        if (g_fontBold)     { DeleteObject(g_fontBold);     g_fontBold     = nullptr; }
        if (g_fontSmall)    { DeleteObject(g_fontSmall);    g_fontSmall    = nullptr; }

        if (g_brBg)         { DeleteObject(g_brBg);         g_brBg         = nullptr; }
        if (g_brCard)       { DeleteObject(g_brCard);       g_brCard       = nullptr; }
        if (g_brBorder)     { DeleteObject(g_brBorder);     g_brBorder     = nullptr; }
        if (g_brNeon)       { DeleteObject(g_brNeon);       g_brNeon       = nullptr; }
        if (g_hLogoHeader)  { DestroyIcon(g_hLogoHeader);   g_hLogoHeader  = nullptr; }
        if (g_tipGpuHelp)   { DestroyWindow(g_tipGpuHelp);  g_tipGpuHelp   = nullptr; }

        PostQuitMessage(0);
        break;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// Ensure nvofapi64.dll is available (auto-copy from System32 if missing)
// ---------------------------------------------------------------------------
static void EnsureNvofapiAvailable()
{
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    PathRemoveFileSpecW(exePath);

    wchar_t targetNvof[MAX_PATH] = {};
    PathCombineW(targetNvof, exePath, L"nvofapi64.dll");

    wchar_t sysDir[MAX_PATH] = {};
    GetSystemDirectoryW(sysDir, MAX_PATH);
    wchar_t srcNvof[MAX_PATH] = {};
    PathCombineW(srcNvof, sysDir, L"nvofapi64.dll");

    // If nvofapi64.dll is missing in the .exe directory, automatically copy from System32
    if (GetFileAttributesW(targetNvof) == INVALID_FILE_ATTRIBUTES)
    {
        if (GetFileAttributesW(srcNvof) != INVALID_FILE_ATTRIBUTES)
        {
            if (CopyFileW(srcNvof, targetNvof, FALSE))
            {
                DLSS_Log("[Init] nvofapi64.dll System32'den basariyla uygulama klasorune kopyalandi.");
            }
            else
            {
                DLSS_Log("[Init] nvofapi64.dll kopyalanamadi (hata=%lu), dogrudan System32'den yuklenecek.", GetLastError());
            }
        }
        else
        {
            DLSS_Log("[Init] Bilgi: System32 altinda nvofapi64.dll bulunamadi.");
        }
    }

    // Preload nvofapi64.dll into process memory
    HMODULE hNvof = LoadLibraryW(targetNvof);
    if (!hNvof && GetFileAttributesW(srcNvof) != INVALID_FILE_ATTRIBUTES)
    {
        hNvof = LoadLibraryW(srcNvof);
    }
    if (hNvof)
    {
        DLSS_Log("[Init] nvofapi64.dll basariyla bellege yuklendi (0x%p).", hNvof);
    }
}

// ---------------------------------------------------------------------------
// WinMain
// ---------------------------------------------------------------------------
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow)
{
    // Single instance check: prevent multiple instances of VLSS5 from running simultaneously
    HANDLE hSingleInstanceMutex = CreateMutexW(nullptr, TRUE, L"Local\\VLSS5_SingleInstance_Mutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS || !hSingleInstanceMutex)
    {
        if (hSingleInstanceMutex)
        {
            CloseHandle(hSingleInstanceMutex);
        }

        // Bring existing window to front if it's visible
        HWND existingHwnd = FindWindowW(L"VLSS5Main", nullptr);
        if (existingHwnd && IsWindow(existingHwnd))
        {
            if (IsIconic(existingHwnd))
            {
                ShowWindow(existingHwnd, SW_RESTORE);
            }
            SetForegroundWindow(existingHwnd);
        }

        MessageBoxW(
            nullptr,
            L"VLSS5 zaten açık, lütfen önce VLSS5'i kapatın ve yeniden deneyin.",
            L"VLSS5",
            MB_ICONWARNING | MB_OK | MB_TOPMOST);
        return 0;
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    winrt::init_apartment();

    // Disable OS background frame-rate throttling
    {
        PROCESS_POWER_THROTTLING_STATE ppt = {};
        ppt.Version     = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        ppt.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
        ppt.StateMask   = 0;
        SetProcessInformation(GetCurrentProcess(),
            ProcessPowerThrottling, &ppt, sizeof(ppt));
    }
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);

    // Automatically ensure nvofapi64.dll is present and preloaded
    EnsureNvofapiAvailable();

    // Automatically ensure _nvngx.dll and nvngx.dll are present and up to date
    EnsureNGXAvailable();

    DLSS_Log("[Main] VLSS5 launcher started.");

    // Register main window class with custom V5 icon
    HICON hMainIcon   = reinterpret_cast<HICON>(LoadImageW(
        hInstance, MAKEINTRESOURCEW(IDI_MAIN_ICON), IMAGE_ICON,
        GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
    HICON hMainIconSm = reinterpret_cast<HICON>(LoadImageW(
        hInstance, MAKEINTRESOURCEW(IDI_MAIN_ICON), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    if (!hMainIcon)   hMainIcon   = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_MAIN_ICON));
    if (!hMainIconSm) hMainIconSm = hMainIcon;

    WNDCLASSEXW wc   = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"VLSS5Main";
    wc.hIcon         = hMainIcon;
    wc.hIconSm       = hMainIconSm;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    // Initialize DLSS 5 settings window class and common controls
    SettingsWindow::Initialize(hInstance);

    // Create modern dark window (fixed size, centered, styled)
    HWND hwnd = CreateWindowExW(
        0,
        L"VLSS5Main",
        L"VLSS5 — Yüksek Performanslı Oyun Overlay",
        (WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX)),
        CW_USEDEFAULT, CW_USEDEFAULT,
        622, 555,
        nullptr, nullptr, hInstance, nullptr);

    if (!hwnd)
    {
        MessageBoxW(nullptr, L"Pencere oluşturulamadı.", L"VLSS5", MB_ICONERROR);
        return -1;
    }

    // Windows 11 / Modern Dark Title Bar & Rounded Corners
    BOOL darkMode = TRUE;
    DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &darkMode, sizeof(darkMode));
    DWORD cornerPref = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(hwnd, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &cornerPref, sizeof(cornerPref));

    if (hMainIcon)
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(hMainIcon));
    if (hMainIconSm)
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(hMainIconSm));

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    // Create App instance
    g_app = std::make_unique<App>(hInstance);

    // Standard Win32 message loop
    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_app.reset();
    winrt::uninit_apartment();

    if (hSingleInstanceMutex)
    {
        ReleaseMutex(hSingleInstanceMutex);
        CloseHandle(hSingleInstanceMutex);
    }

    return static_cast<int>(msg.wParam);
}
