#include "Common.h"
#include "App.h"
#include "WindowEnumerator.h"
#include "DLSSManager.h"
#include "ConfigManager.h"
#include "SettingsWindow.h"
#include "resource.h"

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
#define ID_GLOBAL_HOTKEY      201   // Global capture toggle hotkey
#define IDT_HOTKEY_TIMER      301   // Fallback hotkey poller (50ms)

// ---------------------------------------------------------------------------
// Modern Neon Green Theme Palette
// ---------------------------------------------------------------------------
static const COLORREF COLOR_BG          = RGB(12, 16, 23);     // Deep midnight charcoal
static const COLORREF COLOR_CARD_BG     = RGB(22, 28, 38);     // Elevated dark surface
static const COLORREF COLOR_BORDER      = RGB(36, 46, 62);     // Subtle surface border
static const COLORREF COLOR_NEON_GREEN  = RGB(0, 255, 60);     // Electric neon green
static const COLORREF COLOR_NEON_HOVER  = RGB(60, 255, 110);   // Lighter neon hover
static const COLORREF COLOR_NEON_DARK   = RGB(0, 190, 45);     // Pressed neon green
static const COLORREF COLOR_TEXT_MAIN   = RGB(240, 246, 252);  // Crisp white
static const COLORREF COLOR_TEXT_MUTED  = RGB(139, 148, 158);  // Slate secondary text
static const COLORREF COLOR_LIST_SEL    = RGB(16, 45, 20);     // Electric green selection tint

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
static std::unique_ptr<App>    g_app;
static std::vector<WindowInfo> g_windows;

static HWND                    g_mainHwnd        = nullptr;
static HWND                    g_listBox         = nullptr;
static HWND                    g_lblStatus       = nullptr;
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

        // ---- Window List Section ----
        HWND lblList = CreateWindowW(L"STATIC", L"HEDEF UYGULAMA SEÇİMİ",
            WS_CHILD | WS_VISIBLE, 28, 76, 300, 20,
            hwnd, nullptr, nullptr, nullptr);
        SF(lblList, g_fontBold);

        // Owner-drawn ListBox with dark background & neon highlights
        g_listBox = CreateWindowExW(0, L"LISTBOX", nullptr,
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT,
            28, 102, 534, 175,
            hwnd, reinterpret_cast<HMENU>(IDC_WINDOWLIST), nullptr, nullptr);
        SF(g_listBox, g_fontNormal);
        SendMessageW(g_listBox, LB_SETITEMHEIGHT, 0, 30);

        // ---- Refresh Button ----
        g_btnRefresh = CreateWindowW(L"BUTTON", L"Yenile",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            28, 285, 80, 34,
            hwnd, reinterpret_cast<HMENU>(IDC_BTN_REFRESH), nullptr, nullptr);
        SF(g_btnRefresh, g_fontBold);

        // ---- VSync Switch Checkbox ----
        g_chkVSync = CreateWindowW(L"BUTTON", L" VSync",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            118, 290, 85, 24,
            hwnd, reinterpret_cast<HMENU>(IDC_CHK_VSYNC), nullptr, nullptr);
        SF(g_chkVSync, g_fontNormal);
        Button_SetCheck(g_chkVSync, g_vsyncEnabled ? BST_CHECKED : BST_UNCHECKED);

        // ---- FPS Overlay Switch Checkbox ----
        g_chkFps = CreateWindowW(L"BUTTON", L" FPS Göstergesi",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            210, 290, 130, 24,
            hwnd, reinterpret_cast<HMENU>(IDC_CHK_FPS), nullptr, nullptr);
        SF(g_chkFps, g_fontNormal);
        Button_SetCheck(g_chkFps, g_fpsEnabled ? BST_CHECKED : BST_UNCHECKED);

        // ---- DLSS 5 Switch Checkbox ----
        g_chkDlss = CreateWindowW(L"BUTTON", L" DLSS 5 (Nöral)",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            350, 290, 210, 24,
            hwnd, reinterpret_cast<HMENU>(IDC_CHK_DLSS), nullptr, nullptr);
        SF(g_chkDlss, g_fontNormal);
        Button_SetCheck(g_chkDlss, g_dlssEnabled ? BST_CHECKED : BST_UNCHECKED);

        // ---- Keybind Panel Labels & DLSS 5 Settings Button ----
        HWND lblKeybindTitle = CreateWindowW(L"STATIC", L"Overlay Kısayolu:",
            WS_CHILD | WS_VISIBLE, 28, 350, 130, 22,
            hwnd, nullptr, nullptr, nullptr);
        SF(lblKeybindTitle, g_fontNormal);

        g_lblKeybind = CreateWindowW(L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | SS_CENTER,
            162, 346, 105, 30,
            hwnd, reinterpret_cast<HMENU>(IDC_LBL_KEYBIND), nullptr, nullptr);
        SF(g_lblKeybind, g_fontBold);
        UpdateKeybindLabel();

        g_btnKeybind = CreateWindowW(L"BUTTON", L"Değiştir",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            272, 346, 78, 30,
            hwnd, reinterpret_cast<HMENU>(IDC_BTN_KEYBIND), nullptr, nullptr);
        SF(g_btnKeybind, g_fontNormal);

        g_btnDlssSettings = CreateWindowW(L"BUTTON", L"⚙ DLSS 5 Ayarları",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            358, 346, 202, 30,
            hwnd, reinterpret_cast<HMENU>(IDC_BTN_DLSS_SETTINGS), nullptr, nullptr);
        SF(g_btnDlssSettings, g_fontBold);

        // ---- Start Button (Big Neon Action Button) ----
        g_btnStart = CreateWindowW(L"BUTTON", L"BAŞLAT  ►",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            28, 400, 534, 46,
            hwnd, reinterpret_cast<HMENU>(IDC_BTN_START), nullptr, nullptr);
        SF(g_btnStart, g_fontTitle);

        // ---- Status bar ----
        g_lblStatus = CreateWindowW(L"STATIC", L"Hedef uygulamayı seçin veya istediğiniz penceredeyken ALT+S basın.",
            WS_CHILD | WS_VISIBLE, 28, 458, 534, 20,
            hwnd, reinterpret_cast<HMENU>(IDC_LBL_STATUS), nullptr, nullptr);
        SF(g_lblStatus, g_fontSmall);

        // Global hotkey registration & fallback timer
        RegisterAppHotkey(hwnd);
        SetTimer(hwnd, IDT_HOTKEY_TIMER, 50, nullptr);

        // Populate initial list
        PopulateList(hwnd);
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

        // Header bottom accent line (Neon Green)
        HPEN hPenNeon = CreatePen(PS_SOLID, 2, COLOR_NEON_GREEN);
        HGDIOBJ oldPen = SelectObject(hdc, hPenNeon);
        MoveToEx(hdc, 0, 65, nullptr);
        LineTo(hdc, client.right, 65);
        SelectObject(hdc, oldPen);
        DeleteObject(hPenNeon);

        // Header Emblem / Logo (Neon Green V5)
        if (g_hLogoHeader)
        {
            DrawIconEx(hdc, 20, 10, g_hLogoHeader, 46, 46, 0, nullptr, DI_NORMAL);
        }

        // Header Title (Neon Green)
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, COLOR_NEON_GREEN);
        SelectObject(hdc, g_fontTitle);
        TextOutW(hdc, 76, 14, L"VLSS5", 5);

        // Header Subtitle
        SetTextColor(hdc, COLOR_TEXT_MUTED);
        SelectObject(hdc, g_fontSubtitle);
        TextOutW(hdc, 76, 40, L"Her yerde VLSS5 kullanabilirsiniz!", 54);

        // Keyboard Tips in Header
        SetTextColor(hdc, COLOR_TEXT_MUTED);
        SelectObject(hdc, g_fontSmall);
        TextOutW(hdc, 315, 24, L"[F8] Odakla |  [F9] FPS  |  [F10] VLSS5  |  [Alt+S] Uygula", 52);

        // 3. Card Panel behind Keybind row
        RECT cardKeybind = { 20, 336, 570, 386 };
        DrawModernPanel(hdc, cardKeybind, COLOR_CARD_BG, COLOR_BORDER, 8);

        EndPaint(hwnd, &ps);
        break;
    }

    case WM_DRAWITEM:
    {
        auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
        if (!dis) break;

        // Custom draw the Window ListBox
        if (dis->CtlID == IDC_WINDOWLIST)
        {
            if (dis->itemID == static_cast<UINT>(-1)) break;

            bool isSelected = (dis->itemState & ODS_SELECTED);
            COLORREF itemBg = isSelected ? COLOR_LIST_SEL : COLOR_CARD_BG;
            COLORREF textColor = isSelected ? COLOR_NEON_GREEN : COLOR_TEXT_MAIN;

            HBRUSH brItem = CreateSolidBrush(itemBg);
            FillRect(dis->hDC, &dis->rcItem, brItem);
            DeleteObject(brItem);

            if (isSelected)
            {
                RECT ind = dis->rcItem;
                ind.right = ind.left + 4;
                HBRUSH brNeon = CreateSolidBrush(COLOR_NEON_GREEN);
                FillRect(dis->hDC, &ind, brNeon);
                DeleteObject(brNeon);
            }

            wchar_t text[512] = {};
            SendMessageW(dis->hwndItem, LB_GETTEXT, dis->itemID, reinterpret_cast<LPARAM>(text));

            SetBkMode(dis->hDC, TRANSPARENT);
            SetTextColor(dis->hDC, textColor);
            SelectObject(dis->hDC, g_fontNormal);

            RECT textRc = dis->rcItem;
            textRc.left += isSelected ? 12 : 8;
            DrawTextW(dis->hDC, text, -1, &textRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

            HPEN hPenLine = CreatePen(PS_SOLID, 1, RGB(26, 34, 46));
            HGDIOBJ oldPen = SelectObject(dis->hDC, hPenLine);
            MoveToEx(dis->hDC, dis->rcItem.left, dis->rcItem.bottom - 1, nullptr);
            LineTo(dis->hDC, dis->rcItem.right, dis->rcItem.bottom - 1);
            SelectObject(dis->hDC, oldPen);
            DeleteObject(hPenLine);

            return TRUE;
        }

        // Custom draw Start Button (Neon Glow)
        if (dis->CtlID == IDC_BTN_START)
        {
            bool isPressed = (dis->itemState & ODS_SELECTED);
            COLORREF btnBg = isPressed ? COLOR_NEON_DARK : COLOR_NEON_GREEN;

            DrawModernPanel(dis->hDC, dis->rcItem, btnBg, btnBg, 8);

            SetBkMode(dis->hDC, TRANSPARENT);
            SetTextColor(dis->hDC, RGB(10, 16, 20));
            SelectObject(dis->hDC, g_fontTitle);

            DrawTextW(dis->hDC, L"BAŞLAT  ►", -1, &dis->rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            return TRUE;
        }

        // Custom draw Secondary Buttons
        if (dis->CtlID == IDC_BTN_REFRESH || dis->CtlID == IDC_BTN_KEYBIND || dis->CtlID == IDC_BTN_DLSS_SETTINGS)
        {
            bool isPressed = (dis->itemState & ODS_SELECTED);
            COLORREF btnBg = isPressed ? RGB(32, 42, 58) : COLOR_CARD_BG;
            COLORREF btnBorder = (dis->CtlID == IDC_BTN_DLSS_SETTINGS || isPressed) ? COLOR_NEON_GREEN : COLOR_BORDER;

            DrawModernPanel(dis->hDC, dis->rcItem, btnBg, btnBorder, 6);

            wchar_t btnText[64] = {};
            GetWindowTextW(dis->hwndItem, btnText, _countof(btnText));

            SetBkMode(dis->hDC, TRANSPARENT);
            SetTextColor(dis->hDC, dis->CtlID == IDC_BTN_DLSS_SETTINGS ? COLOR_NEON_GREEN : COLOR_TEXT_MAIN);
            SelectObject(dis->hDC, g_fontBold);

            DrawTextW(dis->hDC, btnText, -1, &dis->rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            return TRUE;
        }
        break;
    }

    case WM_CTLCOLORSTATIC:
    {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        HWND ctlHwnd = reinterpret_cast<HWND>(lParam);

        SetBkMode(hdc, TRANSPARENT);

        if (ctlHwnd == g_lblKeybind)
        {
            SetTextColor(hdc, COLOR_NEON_GREEN);
            return reinterpret_cast<LRESULT>(g_brCard);
        }

        if (ctlHwnd == g_lblStatus)
        {
            SetTextColor(hdc, COLOR_NEON_GREEN);
            return reinterpret_cast<LRESULT>(g_brBg);
        }

        if (ctlHwnd == g_chkVSync || ctlHwnd == g_chkFps || ctlHwnd == g_chkDlss)
        {
            SetTextColor(hdc, COLOR_TEXT_MAIN);
            return reinterpret_cast<LRESULT>(g_brBg);
        }

        SetTextColor(hdc, COLOR_TEXT_MAIN);
        return reinterpret_cast<LRESULT>(g_brBg);
    }

    case WM_CTLCOLORLISTBOX:
    {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, COLOR_TEXT_MAIN);
        return reinterpret_cast<LRESULT>(g_brCard);
    }

    case WM_COMMAND:
    {
        int ctlId = LOWORD(wParam);

        // VSync checkbox toggle
        if (ctlId == IDC_CHK_VSYNC && HIWORD(wParam) == BN_CLICKED)
        {
            g_vsyncEnabled = (Button_GetCheck(g_chkVSync) == BST_CHECKED);
            if (g_vsyncEnabled)
                SetStatus(L"VSync etkin: Kareler monitör yenileme hızına kilitlenecek.");
            else
                SetStatus(L"VSync kapalı: Sınırsız kare hızı & en düşük gecikme.");
            break;
        }

        // FPS checkbox toggle
        if (ctlId == IDC_CHK_FPS && HIWORD(wParam) == BN_CLICKED)
        {
            g_fpsEnabled = (Button_GetCheck(g_chkFps) == BST_CHECKED);
            break;
        }

        // DLSS 5 checkbox toggle
        if (ctlId == IDC_CHK_DLSS && HIWORD(wParam) == BN_CLICKED)
        {
            g_dlssEnabled = (Button_GetCheck(g_chkDlss) == BST_CHECKED);
            if (g_dlssEnabled)
                SetStatus(L"DLSS 5 etkin: Native NVIDIA NGX nöral iyileştirme devrede.");
            else
                SetStatus(L"DLSS 5 kapalı: Standart doğrudan görüntü modu.");
            break;
        }

        // Refresh window list
        if (ctlId == IDC_BTN_REFRESH)
        {
            PopulateList(hwnd);
            SetStatus(L"Pencere listesi güncellendi.");
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

        PostQuitMessage(0);
        break;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// WinMain
// ---------------------------------------------------------------------------
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow)
{
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
        606, 528,
        nullptr, nullptr, hInstance, nullptr);

    if (!hwnd)
    {
        MessageBoxW(nullptr, L"Pencere oluşturulamadı.", L"VLSS5", MB_ICONERROR);
        return -1;
    }

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

    return static_cast<int>(msg.wParam);
}
