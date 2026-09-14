#include "HotkeysWindow.h"
#include <commctrl.h>
#include <uxtheme.h>

static constexpr COLORREF COLOR_DARK_BG    = RGB(15, 19, 26);
static constexpr COLORREF COLOR_CARD_BG    = RGB(22, 28, 38);
static constexpr COLORREF COLOR_BORDER     = RGB(38, 48, 65);
static constexpr COLORREF COLOR_NEON_GREEN = RGB(0, 240, 55);
static constexpr COLORREF COLOR_TEXT_MAIN  = RGB(242, 247, 252);
static constexpr COLORREF COLOR_TEXT_TITLE = RGB(220, 245, 230);
static constexpr COLORREF COLOR_TEXT_DIM   = RGB(139, 148, 158);

#define IDC_HK_BTN_CLOSE   700
#define IDC_HK_BTN_SETTING 701
#define IDC_HK_BTN_FG      702
#define IDC_HK_BTN_FOCUS   703
#define IDC_HK_BTN_FPS     704
#define IDC_HK_BTN_VLSS    705
#define IDC_HK_BTN_CALIB   706
#define IDC_HK_BTN_START   707

HWND HotkeysWindow::s_hwnd = nullptr;
HINSTANCE HotkeysWindow::s_hInstance = nullptr;
bool HotkeysWindow::s_rebindingKey = false;
HHOOK HotkeysWindow::s_rebindHook = nullptr;
int HotkeysWindow::s_currentRebindId = -1;

HWND HotkeysWindow::s_lblSettings = nullptr, HotkeysWindow::s_btnSettings = nullptr;
HWND HotkeysWindow::s_lblFg = nullptr, HotkeysWindow::s_btnFg = nullptr;
HWND HotkeysWindow::s_lblFocus = nullptr, HotkeysWindow::s_btnFocus = nullptr;
HWND HotkeysWindow::s_lblFps = nullptr, HotkeysWindow::s_btnFps = nullptr;
HWND HotkeysWindow::s_lblVlss = nullptr, HotkeysWindow::s_btnVlss = nullptr;
HWND HotkeysWindow::s_lblCalib = nullptr, HotkeysWindow::s_btnCalib = nullptr;
HWND HotkeysWindow::s_lblStart = nullptr, HotkeysWindow::s_btnStart = nullptr;
HWND HotkeysWindow::s_btnClose = nullptr;

HFONT HotkeysWindow::s_fontTitle = nullptr;
HFONT HotkeysWindow::s_fontNormal = nullptr;
HFONT HotkeysWindow::s_fontBold = nullptr;

static void DrawModernPanel(HDC hdc, RECT rc, COLORREF bg, COLORREF border, int radius)
{
    HBRUSH hBr = CreateSolidBrush(bg);
    HPEN hPen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ oldBr = SelectObject(hdc, hBr);
    HGDIOBJ oldPen = SelectObject(hdc, hPen);
    RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, radius, radius);
    SelectObject(hdc, oldBr);
    SelectObject(hdc, oldPen);
    DeleteObject(hBr);
    DeleteObject(hPen);
}

void HotkeysWindow::Initialize(HINSTANCE hInstance)
{
    s_hInstance = hInstance;
    
    WNDCLASSEXW wc = { sizeof(WNDCLASSEXW) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"HotkeysWindowClass";
    RegisterClassExW(&wc);

    s_fontTitle = CreateFontW(24, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    s_fontNormal = CreateFontW(14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    s_fontBold = CreateFontW(14, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
}

void HotkeysWindow::Show(HWND parent)
{
    if (s_hwnd)
    {
        ShowWindow(s_hwnd, SW_SHOW);
        SetForegroundWindow(s_hwnd);
        return;
    }

    int width = 500;
    int height = 450; // Taller for more items
    
    RECT rcParent;
    if (parent) GetWindowRect(parent, &rcParent);
    else { rcParent.left = 100; rcParent.top = 100; rcParent.right = 100 + width; rcParent.bottom = 100 + height; }

    int cx = rcParent.left + (rcParent.right - rcParent.left - width) / 2;
    int cy = rcParent.top + (rcParent.bottom - rcParent.top - height) / 2;

    s_hwnd = CreateWindowExW(WS_EX_TOPMOST, L"HotkeysWindowClass", L"Tuşları Değiştir",
        WS_POPUP, cx, cy, width, height, parent, nullptr, s_hInstance, nullptr);
    
    CreateControls(s_hwnd);
    UpdateLabels();
    
    ShowWindow(s_hwnd, SW_SHOW);
    UpdateWindow(s_hwnd);
}

void HotkeysWindow::Hide()
{
    if (s_rebindingKey) EndKeybindCapture(false, 0, 0);
    if (s_hwnd) ShowWindow(s_hwnd, SW_HIDE);
}

bool HotkeysWindow::IsOpen()
{
    return s_hwnd && IsWindowVisible(s_hwnd);
}

static HWND CreateLabel(HWND parent, const wchar_t* text, int y, HFONT font)
{
    HWND h = CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE, 30, y, 150, 20, parent, nullptr, nullptr, nullptr);
    SendMessageW(h, WM_SETFONT, (WPARAM)font, FALSE);
    return h;
}

static HWND CreateRebindButton(HWND parent, int id, int y, HFONT font)
{
    HWND h = CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 200, y - 5, 120, 30, parent, (HMENU)(int_ptr)id, nullptr, nullptr);
    SendMessageW(h, WM_SETFONT, (WPARAM)font, FALSE);
    return h;
}

void HotkeysWindow::CreateControls(HWND hwnd)
{
    s_btnClose = CreateWindowW(L"BUTTON", L"X",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        450, 20, 30, 30, hwnd, (HMENU)IDC_HK_BTN_CLOSE, nullptr, nullptr);
    SendMessageW(s_btnClose, WM_SETFONT, (WPARAM)s_fontBold, FALSE);

    int y = 70;
    int ystep = 45;

    s_lblStart    = CreateLabel(hwnd, L"Uygulamayı Başlat:", y, s_fontBold);
    s_btnStart    = CreateRebindButton(hwnd, IDC_HK_BTN_START, y, s_fontNormal); y += ystep;

    s_lblSettings = CreateLabel(hwnd, L"VLSS5 Ayarları:", y, s_fontBold);
    s_btnSettings = CreateRebindButton(hwnd, IDC_HK_BTN_SETTING, y, s_fontNormal); y += ystep;

    s_lblFg       = CreateLabel(hwnd, L"FG İşaretçi (DEV):", y, s_fontBold);
    s_btnFg       = CreateRebindButton(hwnd, IDC_HK_BTN_FG, y, s_fontNormal); y += ystep;

    s_lblFocus    = CreateLabel(hwnd, L"Oyuna Odaklan:", y, s_fontBold);
    s_btnFocus    = CreateRebindButton(hwnd, IDC_HK_BTN_FOCUS, y, s_fontNormal); y += ystep;

    s_lblFps      = CreateLabel(hwnd, L"FPS Göstergesi:", y, s_fontBold);
    s_btnFps      = CreateRebindButton(hwnd, IDC_HK_BTN_FPS, y, s_fontNormal); y += ystep;

    s_lblVlss     = CreateLabel(hwnd, L"VLSS5 Aç/Kapat:", y, s_fontBold);
    s_btnVlss     = CreateRebindButton(hwnd, IDC_HK_BTN_VLSS, y, s_fontNormal); y += ystep;

    s_lblCalib    = CreateLabel(hwnd, L"FPS Kalibrasyonu:", y, s_fontBold);
    s_btnCalib    = CreateRebindButton(hwnd, IDC_HK_BTN_CALIB, y, s_fontNormal); y += ystep;
}

void HotkeysWindow::UpdateLabels()
{
    if (!s_hwnd) return;
    auto& cfg = ConfigManager::Get().Config();
    
    SetWindowTextW(s_btnStart, Dlss5Config::FormatKey(cfg.vkStart, cfg.modStart).c_str());
    SetWindowTextW(s_btnSettings, Dlss5Config::FormatKey(cfg.settingsVk, cfg.settingsMod).c_str());
    SetWindowTextW(s_btnFg, Dlss5Config::FormatKey(cfg.vkFgIndicator).c_str());
    SetWindowTextW(s_btnFocus, Dlss5Config::FormatKey(cfg.vkFocus).c_str());
    SetWindowTextW(s_btnFps, Dlss5Config::FormatKey(cfg.vkFps).c_str());
    SetWindowTextW(s_btnVlss, Dlss5Config::FormatKey(cfg.vkToggleVlss).c_str());
    SetWindowTextW(s_btnCalib, Dlss5Config::FormatKey(cfg.vkCalib).c_str());
}

LRESULT CALLBACK HotkeysWindow::RebindKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION && s_rebindingKey && (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN))
    {
        auto* kb = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
        UINT vk = kb->vkCode;

        if (vk == VK_ESCAPE)
        {
            EndKeybindCapture(false, 0, 0);
            return 1;
        }
        else if (vk != VK_LSHIFT && vk != VK_RSHIFT && vk != VK_SHIFT &&
                 vk != VK_LCONTROL && vk != VK_RCONTROL && vk != VK_CONTROL &&
                 vk != VK_LMENU && vk != VK_RMENU && vk != VK_MENU)
        {
            UINT mods = 0;
            if (GetAsyncKeyState(VK_CONTROL) & 0x8000) mods |= MOD_CONTROL;
            if (GetAsyncKeyState(VK_MENU)    & 0x8000) mods |= MOD_ALT;
            if (GetAsyncKeyState(VK_SHIFT)   & 0x8000) mods |= MOD_SHIFT;

            EndKeybindCapture(true, vk, mods);
            return 1;
        }
    }
    return CallNextHookEx(s_rebindHook, nCode, wParam, lParam);
}

void HotkeysWindow::StartKeybindCapture(int hotkeyId)
{
    if (s_rebindingKey) return;
    s_currentRebindId = hotkeyId;
    s_rebindingKey = true;
    s_rebindHook = SetWindowsHookExW(WH_KEYBOARD_LL, RebindKeyboardProc, GetModuleHandle(nullptr), 0);
    
    HWND btn = nullptr;
    if (hotkeyId == 0) btn = s_btnSettings;
    if (hotkeyId == 1) btn = s_btnFg;
    if (hotkeyId == 2) btn = s_btnFocus;
    if (hotkeyId == 3) btn = s_btnFps;
    if (hotkeyId == 4) btn = s_btnVlss;
    if (hotkeyId == 5) btn = s_btnCalib;
    if (hotkeyId == 6) btn = s_btnStart;

    if (btn)
    {
        SetWindowTextW(btn, L"Tuşa Basın...");
        InvalidateRect(btn, nullptr, TRUE);
    }
}

void HotkeysWindow::EndKeybindCapture(bool save, UINT vk, UINT mod)
{
    if (!s_rebindingKey) return;
    s_rebindingKey = false;
    if (s_rebindHook)
    {
        UnhookWindowsHookEx(s_rebindHook);
        s_rebindHook = nullptr;
    }

    if (save)
    {
        auto& cfg = ConfigManager::Get().Config();
        if (s_currentRebindId == 0) { cfg.settingsVk = vk; cfg.settingsMod = mod; }
        if (s_currentRebindId == 1) { cfg.vkFgIndicator = vk; }
        if (s_currentRebindId == 2) { cfg.vkFocus = vk; }
        if (s_currentRebindId == 3) { cfg.vkFps = vk; }
        if (s_currentRebindId == 4) { cfg.vkToggleVlss = vk; }
        if (s_currentRebindId == 5) { cfg.vkCalib = vk; }
        if (s_currentRebindId == 6) { cfg.vkStart = vk; cfg.modStart = mod; }

        ConfigManager::Get().Save();
        
        // Let Main window know we updated start key
        HWND hMain = FindWindowW(L"VLSS5Main", nullptr);
        if (hMain) PostMessageW(hMain, WM_APP + 1, 0, 0); // Custom message to re-register hotkey and update label
    }

    UpdateLabels();
    InvalidateRect(s_hwnd, nullptr, TRUE);
}

LRESULT CALLBACK HotkeysWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        DrawModernPanel(hdc, rc, COLOR_DARK_BG, COLOR_NEON_GREEN, 10);
        
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, COLOR_TEXT_TITLE);
        SelectObject(hdc, s_fontTitle);
        rc.top = 20; rc.left = 30;
        DrawTextW(hdc, L"Kısayol Tuşları", -1, &rc, DT_SINGLELINE | DT_LEFT | DT_TOP);
        
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CTLCOLORSTATIC:
    {
        HDC hdc = (HDC)wParam;
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, COLOR_TEXT_MAIN);
        return (LRESULT)GetStockObject(NULL_BRUSH);
    }
    case WM_DRAWITEM:
    {
        auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
        if (!dis) break;

        bool isPressed = (dis->itemState & ODS_SELECTED);
        COLORREF btnBg = COLOR_CARD_BG;
        COLORREF textCol = COLOR_TEXT_MAIN;
        
        if (dis->CtlID == IDC_HK_BTN_CLOSE) {
            btnBg = isPressed ? RGB(200,50,50) : COLOR_CARD_BG;
            DrawModernPanel(dis->hDC, dis->rcItem, btnBg, COLOR_BORDER, 6);
            SetBkMode(dis->hDC, TRANSPARENT);
            SetTextColor(dis->hDC, textCol);
            SelectObject(dis->hDC, s_fontBold);
            DrawTextW(dis->hDC, L"X", -1, &dis->rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            return TRUE;
        }

        if (isPressed || (s_rebindingKey && 
            ((s_currentRebindId == 0 && dis->CtlID == IDC_HK_BTN_SETTING) ||
             (s_currentRebindId == 1 && dis->CtlID == IDC_HK_BTN_FG) ||
             (s_currentRebindId == 2 && dis->CtlID == IDC_HK_BTN_FOCUS) ||
             (s_currentRebindId == 3 && dis->CtlID == IDC_HK_BTN_FPS) ||
             (s_currentRebindId == 4 && dis->CtlID == IDC_HK_BTN_VLSS) ||
             (s_currentRebindId == 5 && dis->CtlID == IDC_HK_BTN_CALIB) ||
             (s_currentRebindId == 6 && dis->CtlID == IDC_HK_BTN_START))))
        {
            btnBg = COLOR_BORDER;
        }

        DrawModernPanel(dis->hDC, dis->rcItem, btnBg, COLOR_BORDER, 6);
        SetBkMode(dis->hDC, TRANSPARENT);
        SetTextColor(dis->hDC, textCol);
        SelectObject(dis->hDC, s_fontNormal);

        wchar_t text[64] = {};
        GetWindowTextW(dis->hwndItem, text, 64);
        DrawTextW(dis->hDC, text, -1, &dis->rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return TRUE;
    }
    case WM_COMMAND:
    {
        int ctlId = LOWORD(wParam);
        if (ctlId == IDC_HK_BTN_CLOSE)
        {
            Hide();
            return 0;
        }
        else if (!s_rebindingKey)
        {
            if (ctlId == IDC_HK_BTN_SETTING) StartKeybindCapture(0);
            if (ctlId == IDC_HK_BTN_FG) StartKeybindCapture(1);
            if (ctlId == IDC_HK_BTN_FOCUS) StartKeybindCapture(2);
            if (ctlId == IDC_HK_BTN_FPS) StartKeybindCapture(3);
            if (ctlId == IDC_HK_BTN_VLSS) StartKeybindCapture(4);
            if (ctlId == IDC_HK_BTN_CALIB) StartKeybindCapture(5);
            if (ctlId == IDC_HK_BTN_START) StartKeybindCapture(6);
        }
        return 0;
    }
    case WM_LBUTTONDOWN:
    {
        SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        return 0;
    }
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}
