#include "SettingsWindow.h"
#include "resource.h"
#include <commctrl.h>
#include <uxtheme.h>
#include <algorithm>
#include <cstdio>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "dwmapi.lib")

static constexpr COLORREF COLOR_DARK_BG    = RGB(15, 19, 26);
static constexpr COLORREF COLOR_CARD_BG    = RGB(22, 28, 38);
static constexpr COLORREF COLOR_BORDER     = RGB(38, 48, 65);
static constexpr COLORREF COLOR_NEON_GREEN = RGB(0, 240, 55);
static constexpr COLORREF COLOR_NEON_DARK  = RGB(0, 180, 40);
static constexpr COLORREF COLOR_TEXT_MAIN  = RGB(242, 247, 252);
static constexpr COLORREF COLOR_TEXT_TITLE = RGB(220, 245, 230);
static constexpr COLORREF COLOR_TEXT_DIM   = RGB(139, 148, 158);

#define IDC_SW_STYLE          501
#define IDC_SW_PRESET         502
#define IDC_SW_SLIDER_INTENSE 503
#define IDC_SW_SLIDER_BOOST   513
#define IDC_SW_SLIDER_STRUCT  504
#define IDC_SW_SLIDER_TONE    505
#define IDC_SW_SLIDER_SKIN    506
#define IDC_SW_SLIDER_RESSCALE 507
#define IDC_SW_SLIDER_PASSES   516
#define IDC_SW_SLIDER_FALLOFF  517
#define IDC_SW_CHK_AUTOMASK   508
#define IDC_SW_CHK_OPTFLOW    511
#define IDC_SW_CHK_SPLIT      514
#define IDC_SW_SLIDER_SPLIT   515
#define IDC_SW_BTN_CLOSE      510

HWND                             SettingsWindow::s_hwnd             = nullptr;
HINSTANCE                        SettingsWindow::s_hInstance        = nullptr;
SettingsWindow::ConfigChangedCallback SettingsWindow::s_callback   = nullptr;

HWND SettingsWindow::s_comboStyle        = nullptr;
HWND SettingsWindow::s_comboPreset       = nullptr;
HWND SettingsWindow::s_sliderIntensity   = nullptr;
HWND SettingsWindow::s_lblIntensityVal   = nullptr;
HWND SettingsWindow::s_sliderBoost       = nullptr;
HWND SettingsWindow::s_lblBoostVal       = nullptr;
HWND SettingsWindow::s_sliderStructure   = nullptr;
HWND SettingsWindow::s_lblStructureVal   = nullptr;
HWND SettingsWindow::s_sliderTone        = nullptr;
HWND SettingsWindow::s_lblToneVal        = nullptr;
HWND SettingsWindow::s_sliderSkin        = nullptr;
HWND SettingsWindow::s_lblSkinVal        = nullptr;
HWND SettingsWindow::s_sliderResScale    = nullptr;
HWND SettingsWindow::s_lblResScaleVal    = nullptr;
HWND SettingsWindow::s_sliderPassCount   = nullptr;
HWND SettingsWindow::s_lblPassCountVal   = nullptr;
HWND SettingsWindow::s_sliderPassFalloff = nullptr;
HWND SettingsWindow::s_lblPassFalloffVal = nullptr;
HWND SettingsWindow::s_chkAutoMask       = nullptr;
HWND SettingsWindow::s_chkOpticalFlow    = nullptr;
HWND SettingsWindow::s_chkSplitScreen    = nullptr;
HWND SettingsWindow::s_lblSplitTitle     = nullptr;
HWND SettingsWindow::s_lblSplitVal       = nullptr;
HWND SettingsWindow::s_sliderSplit       = nullptr;
HWND SettingsWindow::s_btnClose          = nullptr;

HFONT SettingsWindow::s_fontTitle  = nullptr;
HFONT SettingsWindow::s_fontNormal = nullptr;
HFONT SettingsWindow::s_fontBold   = nullptr;
HFONT SettingsWindow::s_fontSmall  = nullptr;
HBRUSH SettingsWindow::s_brBg      = nullptr;
HBRUSH SettingsWindow::s_brCard    = nullptr;
HBRUSH SettingsWindow::s_brBorder  = nullptr;

static void SetFont(HWND h, HFONT f) { SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(f), TRUE); }

void SettingsWindow::Initialize(HINSTANCE hInstance)
{
    s_hInstance = hInstance;

    INITCOMMONCONTROLSEX icc = { sizeof(INITCOMMONCONTROLSEX), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    s_fontTitle  = CreateFontW(-19, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    s_fontBold   = CreateFontW(-13, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    s_fontNormal = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    s_fontSmall  = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

    s_brBg     = CreateSolidBrush(COLOR_DARK_BG);
    s_brCard   = CreateSolidBrush(COLOR_CARD_BG);
    s_brBorder = CreateSolidBrush(COLOR_BORDER);

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(WNDCLASSEXW);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.hIcon         = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_MAIN_ICON));
    wc.hIconSm       = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_MAIN_ICON));
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = s_brBg;
    wc.lpszClassName = L"VLSS5_SettingsWindowClass";
    RegisterClassExW(&wc);

    // Register modern custom controls
    WNDCLASSEXW wcs = {};
    wcs.cbSize        = sizeof(WNDCLASSEXW);
    wcs.style         = CS_HREDRAW | CS_VREDRAW;
    wcs.lpfnWndProc   = ModernSliderProc;
    wcs.hInstance     = hInstance;
    wcs.hCursor       = LoadCursor(nullptr, IDC_HAND);
    wcs.hbrBackground = s_brBg;
    wcs.lpszClassName = L"VLSS5_ModernSlider";
    RegisterClassExW(&wcs);

    WNDCLASSEXW wct = {};
    wct.cbSize        = sizeof(WNDCLASSEXW);
    wct.style         = CS_HREDRAW | CS_VREDRAW;
    wct.lpfnWndProc   = ModernToggleProc;
    wct.hInstance     = hInstance;
    wct.hCursor       = LoadCursor(nullptr, IDC_HAND);
    wct.hbrBackground = s_brBg;
    wct.lpszClassName = L"VLSS5_ModernToggle";
    RegisterClassExW(&wct);
}

void SettingsWindow::Show(HWND parent)
{
    if (!s_hwnd)
    {
        int w = 460;
        int h = 782;
        int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
        int y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;

        s_hwnd = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
            L"VLSS5_SettingsWindowClass",
            L"VLSS5 Nöral Yapılandırma",
            WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
            x, y, w, h,
            parent, nullptr, s_hInstance, nullptr);

        if (s_hwnd)
        {
            HICON hIcon = LoadIconW(s_hInstance, MAKEINTRESOURCEW(IDI_MAIN_ICON));
            if (hIcon)
            {
                SendMessageW(s_hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(hIcon));
                SendMessageW(s_hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(hIcon));
            }

            // Windows 11 / 10 Dark Mode Titlebar & Rounded Corners
            BOOL useDarkMode = TRUE;
            DwmSetWindowAttribute(s_hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &useDarkMode, sizeof(useDarkMode));
            DwmSetWindowAttribute(s_hwnd, 19 /* fallback */, &useDarkMode, sizeof(useDarkMode));
            DWORD cornerPref = 2; // DWMWCP_ROUND
            DwmSetWindowAttribute(s_hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &cornerPref, sizeof(cornerPref));
        }
    }
    else
    {
        ShowWindow(s_hwnd, SW_SHOW);
        SetForegroundWindow(s_hwnd);
    }

    UpdateControlValues();
}

void SettingsWindow::Hide()
{
    if (s_hwnd)
    {
        ShowWindow(s_hwnd, SW_HIDE);
    }
}

void SettingsWindow::Toggle(HWND parent)
{
    if (s_hwnd && IsWindowVisible(s_hwnd))
    {
        Hide();
    }
    else
    {
        Show(parent);
    }
}

bool SettingsWindow::IsOpen()
{
    return (s_hwnd && IsWindowVisible(s_hwnd));
}

void SettingsWindow::SetOnConfigChanged(ConfigChangedCallback cb)
{
    s_callback = cb;
}

struct ModernSliderState
{
    int minVal = 0;
    int maxVal = 100;
    int curVal = 50;
    bool isDragging = false;
};

LRESULT CALLBACK SettingsWindow::ModernSliderProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    auto* state = reinterpret_cast<ModernSliderState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg)
    {
    case WM_NCCREATE:
    {
        state = new ModernSliderState();
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        return TRUE;
    }
    case WM_NCDESTROY:
    {
        delete state;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        return 0;
    }
    case TBM_SETRANGE:
    {
        if (state)
        {
            state->minVal = static_cast<int>(LOWORD(lParam));
            state->maxVal = static_cast<int>(HIWORD(lParam));
            if (state->curVal < state->minVal) state->curVal = state->minVal;
            if (state->curVal > state->maxVal) state->curVal = state->maxVal;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case TBM_SETPOS:
    {
        if (state)
        {
            state->curVal = static_cast<int>(lParam);
            if (state->curVal < state->minVal) state->curVal = state->minVal;
            if (state->curVal > state->maxVal) state->curVal = state->maxVal;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case TBM_GETPOS:
    {
        return state ? state->curVal : 0;
    }
    case TBM_SETTICFREQ:
        return 0;

    case WM_LBUTTONDOWN:
    {
        if (!state) break;
        SetCapture(hwnd);
        state->isDragging = true;

        RECT rc;
        GetClientRect(hwnd, &rc);
        int padding = 8;
        int trackW = rc.right - rc.left - 2 * padding;
        if (trackW > 0)
        {
            int mx = GET_X_LPARAM(lParam);
            float frac = static_cast<float>(mx - padding) / static_cast<float>(trackW);
            frac = std::clamp(frac, 0.0f, 1.0f);
            int newVal = state->minVal + static_cast<int>(frac * (state->maxVal - state->minVal) + 0.5f);
            if (newVal != state->curVal)
            {
                state->curVal = newVal;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            HWND parent = GetParent(hwnd);
            if (parent)
            {
                SendMessageW(parent, WM_HSCROLL, MAKEWPARAM(TB_THUMBTRACK, 0), reinterpret_cast<LPARAM>(hwnd));
            }
        }
        return 0;
    }
    case WM_MOUSEMOVE:
    {
        if (!state || !state->isDragging) break;

        RECT rc;
        GetClientRect(hwnd, &rc);
        int padding = 8;
        int trackW = rc.right - rc.left - 2 * padding;
        if (trackW > 0)
        {
            int mx = GET_X_LPARAM(lParam);
            float frac = static_cast<float>(mx - padding) / static_cast<float>(trackW);
            frac = std::clamp(frac, 0.0f, 1.0f);
            int newVal = state->minVal + static_cast<int>(frac * (state->maxVal - state->minVal) + 0.5f);
            if (newVal != state->curVal)
            {
                state->curVal = newVal;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            HWND parent = GetParent(hwnd);
            if (parent)
            {
                SendMessageW(parent, WM_HSCROLL, MAKEWPARAM(TB_THUMBTRACK, 0), reinterpret_cast<LPARAM>(hwnd));
            }
        }
        return 0;
    }
    case WM_LBUTTONUP:
    {
        if (state && state->isDragging)
        {
            state->isDragging = false;
            ReleaseCapture();
            HWND parent = GetParent(hwnd);
            if (parent)
            {
                SendMessageW(parent, WM_HSCROLL, MAKEWPARAM(TB_ENDTRACK, 0), reinterpret_cast<LPARAM>(hwnd));
            }
        }
        return 0;
    }
    case WM_CAPTURECHANGED:
    {
        if (state) state->isDragging = false;
        return 0;
    }
    case WM_MOUSEWHEEL:
    {
        if (!state) break;
        short delta = GET_WHEEL_DELTA_WPARAM(wParam);
        int step = (std::max)(1, (state->maxVal - state->minVal) / 50);
        if (delta > 0) state->curVal = (std::min)(state->maxVal, state->curVal + step);
        else           state->curVal = (std::max)(state->minVal, state->curVal - step);
        InvalidateRect(hwnd, nullptr, FALSE);
        HWND parent = GetParent(hwnd);
        if (parent)
        {
            SendMessageW(parent, WM_HSCROLL, MAKEWPARAM(TB_THUMBTRACK, 0), reinterpret_cast<LPARAM>(hwnd));
            SendMessageW(parent, WM_HSCROLL, MAKEWPARAM(TB_ENDTRACK, 0), reinterpret_cast<LPARAM>(hwnd));
        }
        return 0;
    }
    case WM_PAINT:
    {
        if (!state) break;
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        int w = rc.right - rc.left;
        int h = rc.bottom - rc.top;

        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP memBmp = CreateCompatibleBitmap(hdc, w, h);
        HGDIOBJ oldBmp = SelectObject(memDC, memBmp);

        // Fill background
        FillRect(memDC, &rc, s_brBg);

        int padding = 8;
        int trackH = 4;
        int trackY = (h - trackH) / 2;
        int trackW = w - 2 * padding;

        if (trackW > 0)
        {
            // 1. Horizontal trackbar channel (white etched bar matching user screenshot)
            RECT rcTrack = { padding, trackY, padding + trackW, trackY + trackH };
            DrawEdge(memDC, &rcTrack, EDGE_SUNKEN, BF_RECT);

            float frac = (state->maxVal > state->minVal)
                ? static_cast<float>(state->curVal - state->minVal) / static_cast<float>(state->maxVal - state->minVal)
                : 0.0f;
            frac = std::clamp(frac, 0.0f, 1.0f);
            int thumbX = padding + static_cast<int>(frac * trackW);

            // 2. Glowing neon thumb capsule
            int thumbW = 10;
            int thumbH = 20;
            int thumbLeft = thumbX - thumbW / 2;
            int thumbTop = (h - thumbH) / 2;

            HPEN penGlow = CreatePen(PS_SOLID, 1, RGB(80, 255, 120));
            HBRUSH brThumb = CreateSolidBrush(RGB(0, 255, 60));
            HGDIOBJ oldPen = SelectObject(memDC, penGlow);
            HGDIOBJ oldBr  = SelectObject(memDC, brThumb);

            RoundRect(memDC, thumbLeft, thumbTop, thumbLeft + thumbW, thumbTop + thumbH, 6, 6);

            SelectObject(memDC, oldBr);
            SelectObject(memDC, oldPen);
            DeleteObject(penGlow);
            DeleteObject(brThumb);
        }

        BitBlt(hdc, 0, 0, w, h, memDC, 0, 0, SRCCOPY);

        SelectObject(memDC, oldBmp);
        DeleteObject(memBmp);
        DeleteDC(memDC);

        EndPaint(hwnd, &ps);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

struct ModernToggleState
{
    bool checked = false;
    std::wstring text;
};

LRESULT CALLBACK SettingsWindow::ModernToggleProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    auto* state = reinterpret_cast<ModernToggleState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg)
    {
    case WM_NCCREATE:
    {
        state = new ModernToggleState();
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        if (cs && cs->lpszName) state->text = cs->lpszName;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        return TRUE;
    }
    case WM_NCDESTROY:
    {
        delete state;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        return 0;
    }
    case BM_GETCHECK:
    {
        return state ? (state->checked ? BST_CHECKED : BST_UNCHECKED) : BST_UNCHECKED;
    }
    case BM_SETCHECK:
    {
        if (state)
        {
            state->checked = (wParam == BST_CHECKED);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_SETTEXT:
    {
        if (state && lParam)
        {
            state->text = reinterpret_cast<const wchar_t*>(lParam);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return TRUE;
    }
    case WM_LBUTTONDOWN:
    {
        if (!state) break;
        state->checked = !state->checked;
        InvalidateRect(hwnd, nullptr, FALSE);
        HWND parent = GetParent(hwnd);
        if (parent)
        {
            SendMessageW(parent, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(hwnd), BN_CLICKED), reinterpret_cast<LPARAM>(hwnd));
        }
        return 0;
    }
    case WM_PAINT:
    {
        if (!state) break;
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        int w = rc.right - rc.left;
        int h = rc.bottom - rc.top;

        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP memBmp = CreateCompatibleBitmap(hdc, w, h);
        HGDIOBJ oldBmp = SelectObject(memDC, memBmp);

        // Fill background
        FillRect(memDC, &rc, s_brBg);

        // 1. Label on the left
        SetBkMode(memDC, TRANSPARENT);
        SetTextColor(memDC, COLOR_TEXT_MAIN);
        SelectObject(memDC, s_fontBold);
        RECT rcText = { 0, 0, w - 50, h };
        DrawTextW(memDC, state->text.c_str(), -1, &rcText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        // 2. Switch Pill on the right
        int pillW = 42;
        int pillH = 22;
        int pillX = w - pillW;
        int pillY = (h - pillH) / 2;

        HPEN penNull = CreatePen(PS_NULL, 0, 0);
        HGDIOBJ oldPen = SelectObject(memDC, penNull);

        if (state->checked)
        {
            // ON: Glowing neon green pill
            HBRUSH brActive = CreateSolidBrush(RGB(0, 230, 55));
            HGDIOBJ oldBr = SelectObject(memDC, brActive);
            RoundRect(memDC, pillX, pillY, pillX + pillW, pillY + pillH, pillH, pillH);

            // Bright white circle knob on the right
            int knobD = 18;
            int knobX = pillX + pillW - knobD - 2;
            int knobY = pillY + 2;
            HBRUSH brKnob = CreateSolidBrush(RGB(255, 255, 255));
            SelectObject(memDC, brKnob);
            RoundRect(memDC, knobX, knobY, knobX + knobD, knobY + knobD, knobD, knobD);

            SelectObject(memDC, oldBr);
            DeleteObject(brKnob);
            DeleteObject(brActive);
        }
        else
        {
            // OFF: Dark slate pill with border
            HPEN penBorder = CreatePen(PS_SOLID, 1, RGB(55, 65, 82));
            HBRUSH brInactive = CreateSolidBrush(RGB(30, 38, 50));
            SelectObject(memDC, penBorder);
            HGDIOBJ oldBr = SelectObject(memDC, brInactive);

            RoundRect(memDC, pillX, pillY, pillX + pillW, pillY + pillH, pillH, pillH);

            // Slate-grey knob on the left
            int knobD = 18;
            int knobX = pillX + 2;
            int knobY = pillY + 2;
            SelectObject(memDC, penNull);
            HBRUSH brKnob = CreateSolidBrush(RGB(130, 140, 155));
            SelectObject(memDC, brKnob);
            RoundRect(memDC, knobX, knobY, knobX + knobD, knobY + knobD, knobD, knobD);

            SelectObject(memDC, oldBr);
            DeleteObject(brKnob);
            DeleteObject(brInactive);
            DeleteObject(penBorder);
        }

        SelectObject(memDC, oldPen);
        DeleteObject(penNull);

        BitBlt(hdc, 0, 0, w, h, memDC, 0, 0, SRCCOPY);

        SelectObject(memDC, oldBmp);
        DeleteObject(memBmp);
        DeleteDC(memDC);

        EndPaint(hwnd, &ps);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void SettingsWindow::CreateControls(HWND hwnd)
{
    // --- Header ---
    HWND lblTitle = CreateWindowW(L"STATIC", L"VLSS5 NÖRAL AYARLAR",
        WS_CHILD | WS_VISIBLE, 24, 16, 400, 24, hwnd, nullptr, nullptr, nullptr);
    SetFont(lblTitle, s_fontTitle);

    HWND lblSubtitle = CreateWindowW(L"STATIC", L"VLSS5 Gelişmiş Canlı Yapılandırma Paneli",
        WS_CHILD | WS_VISIBLE, 24, 40, 400, 18, hwnd, nullptr, nullptr, nullptr);
    SetFont(lblSubtitle, s_fontSmall);

    int y = 78;

    // --- Dropdowns Row: Style & Preset ---
    HWND lblStyle = CreateWindowW(L"STATIC", L"Görsel Stil (Style):",
        WS_CHILD | WS_VISIBLE, 24, y, 180, 18, hwnd, nullptr, nullptr, nullptr);
    SetFont(lblStyle, s_fontBold);

    HWND lblPreset = CreateWindowW(L"STATIC", L"Model Varyantı (Preset):",
        WS_CHILD | WS_VISIBLE, 230, y, 180, 18, hwnd, nullptr, nullptr, nullptr);
    SetFont(lblPreset, s_fontBold);

    y += 22;

    s_comboStyle = CreateWindowW(L"COMBOBOX", nullptr,
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        24, y, 190, 140, hwnd, reinterpret_cast<HMENU>(IDC_SW_STYLE), s_hInstance, nullptr);
    SetFont(s_comboStyle, s_fontNormal);
    SetWindowTheme(s_comboStyle, L"DarkMode_Explorer", nullptr);
    SendMessageW(s_comboStyle, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Standard (Keskin)"));
    SendMessageW(s_comboStyle, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Natural (Doğal)"));
    SendMessageW(s_comboStyle, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Cinematic (Film)"));

    s_comboPreset = CreateWindowW(L"COMBOBOX", nullptr,
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        230, y, 190, 140, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SW_PRESET)), s_hInstance, nullptr);
    SetFont(s_comboPreset, s_fontNormal);
    SetWindowTheme(s_comboPreset, L"DarkMode_Explorer", nullptr);
    SendMessageW(s_comboPreset, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Default (Önerilen)"));
    SendMessageW(s_comboPreset, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Preset 1"));
    SendMessageW(s_comboPreset, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Preset 2"));
    SendMessageW(s_comboPreset, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Preset 3"));

    y += 38;

    // --- Sliders Helper Macro ---
    auto createSliderRow = [&](const wchar_t* title, HWND& slider, HWND& valLabel, INT_PTR id, int minV, int maxV) {
        HWND lbl = CreateWindowW(L"STATIC", title,
            WS_CHILD | WS_VISIBLE, 24, y, 280, 18, hwnd, nullptr, nullptr, nullptr);
        SetFont(lbl, s_fontBold);

        valLabel = CreateWindowW(L"STATIC", L"1.00x",
            WS_CHILD | WS_VISIBLE | SS_RIGHT, 320, y, 100, 18, hwnd, nullptr, nullptr, nullptr);
        SetFont(valLabel, s_fontBold);

        y += 20;

        slider = CreateWindowExW(0, L"VLSS5_ModernSlider", nullptr,
            WS_CHILD | WS_VISIBLE,
            20, y, 405, 26, hwnd, reinterpret_cast<HMENU>(id), s_hInstance, nullptr);
        SendMessageW(slider, TBM_SETRANGE, TRUE, MAKELPARAM(minV, maxV));

        y += 34;
    };

    createSliderRow(L"Nöral Şiddet (Intensity):", s_sliderIntensity, s_lblIntensityVal, IDC_SW_SLIDER_INTENSE, 0, 100);
    createSliderRow(L"Nöral Etki Yoğunluğu (Boost):", s_sliderBoost, s_lblBoostVal, IDC_SW_SLIDER_BOOST, 100, 250);
    createSliderRow(L"Yüzey Detayı (Local Structure):", s_sliderStructure, s_lblStructureVal, IDC_SW_SLIDER_STRUCT, 0, 200);
    createSliderRow(L"Mikro Kontrast (Local Tone):", s_sliderTone, s_lblToneVal, IDC_SW_SLIDER_TONE, 0, 200);
    createSliderRow(L"Ten Doku Ayarı (Skin Structure):", s_sliderSkin, s_lblSkinVal, IDC_SW_SLIDER_SKIN, 0, 300);
    createSliderRow(L"Model Çözünürlüğü (Performans):", s_sliderResScale, s_lblResScaleVal, IDC_SW_SLIDER_RESSCALE, 50, 100);

    // Çok geçişli işleme: model kendi çıktısını tekrar girdi olarak alır.
    // Maliyeti geçiş sayısıyla DOĞRUSAL artar (3 geçiş ~ 3x model süresi), o
    // yüzden pahalı slider listesine alınıp yalnızca bırakıldığında uygulanır.
    createSliderRow(L"Geçiş Sayısı (Multipass):", s_sliderPassCount, s_lblPassCountVal, IDC_SW_SLIDER_PASSES, 1, 4);
    createSliderRow(L"Geçiş Zayıflaması (Falloff):", s_sliderPassFalloff, s_lblPassFalloffVal, IDC_SW_SLIDER_FALLOFF, 25, 100);

    y += 4;

    // --- Modern Switch Toggles (Sadece Modern Switch, Checkbox Yok) ---
    s_chkAutoMask = CreateWindowW(L"VLSS5_ModernToggle", L"Otomatik Ten Tespiti (Auto Skin Mask)",
        WS_CHILD | WS_VISIBLE,
        24, y, 396, 26, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SW_CHK_AUTOMASK)), s_hInstance, nullptr);

    y += 30;

    // DIKKAT: bu toggle cfg.opticalFlow'u surer, cfg.temporalStabilizer'i DEGIL.
    // Eskiden "Golge Bozulma Onleyici" etiketi tasiyordu ve hangi alani yazdigi
    // etiketinden anlasilmiyordu; ayni anda temporalStabilizer UI'dan erisilemez
    // durumdaydi. Etiket artik yaptigi isi soyluyor.
    // ACIK  -> optik akis MV'leri uretilir, DLSS-NR reset=0 ile temporal birikim yapar.
    // KAPALI -> MV yok, reset=1 her kare; temporal birikim tamamen kalkar.
    s_chkOpticalFlow = CreateWindowW(L"VLSS5_ModernToggle", L"Optik Akış Hareket Takibi (Motion Vectors)",
        WS_CHILD | WS_VISIBLE,
        24, y, 396, 26, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SW_CHK_OPTFLOW)), s_hInstance, nullptr);

    y += 30;

    s_chkSplitScreen = CreateWindowW(L"VLSS5_ModernToggle", L"Bölünmüş Ekran (Karşılaştırma / Split)",
        WS_CHILD | WS_VISIBLE,
        24, y, 396, 26, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SW_CHK_SPLIT)), s_hInstance, nullptr);

    y += 30;

    // --- Split Slider Row (Aktifse hemen altına kaydırıcı gelir) ---
    s_lblSplitTitle = CreateWindowW(L"STATIC", L"Bölünme Çizgisi (Sol: Ham | Sağ: DLSS 5):",
        WS_CHILD, 24, y, 290, 18, hwnd, nullptr, nullptr, nullptr);
    SetFont(s_lblSplitTitle, s_fontBold);

    s_lblSplitVal = CreateWindowW(L"STATIC", L"%50",
        WS_CHILD | SS_RIGHT, 320, y, 100, 18, hwnd, nullptr, nullptr, nullptr);
    SetFont(s_lblSplitVal, s_fontBold);

    y += 20;

    s_sliderSplit = CreateWindowExW(0, L"VLSS5_ModernSlider", nullptr,
        WS_CHILD,
        20, y, 405, 26, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SW_SLIDER_SPLIT)), s_hInstance, nullptr);
    SendMessageW(s_sliderSplit, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
    SendMessageW(s_sliderSplit, TBM_SETPOS, TRUE, 50);

    y += 34;

    // --- Close Button ---
    s_btnClose = CreateWindowW(L"BUTTON", L"KAPAT / UYGULA",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        24, y, 396, 36, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SW_BTN_CLOSE)), s_hInstance, nullptr);
    SetFont(s_btnClose, s_fontBold);
}

void SettingsWindow::UpdateLayout()
{
    if (!s_hwnd || !s_chkSplitScreen) return;

    bool splitActive = (Button_GetCheck(s_chkSplitScreen) == BST_CHECKED);

    ShowWindow(s_lblSplitTitle, splitActive ? SW_SHOW : SW_HIDE);
    ShowWindow(s_lblSplitVal,   splitActive ? SW_SHOW : SW_HIDE);
    ShowWindow(s_sliderSplit,   splitActive ? SW_SHOW : SW_HIDE);

    // Toggle sirasi: AutoMask -> OpticalFlow -> Split (her biri 30 px).
    // 576 -> 684: Multipass ve Falloff satirlari (2 x 54 px) yukarida eklendi.
    // 684 -> 654: Overlay kompozisyon satiri (34 px) ve Zamansal Sabitleyici
    //             toggle'i (30 px) arayuzden kaldirildi.
    const int chkSplitY = 654;

    if (splitActive)
    {
        SetWindowPos(s_lblSplitTitle, nullptr, 24, chkSplitY + 30, 290, 18, SWP_NOZORDER | SWP_NOACTIVATE);
        SetWindowPos(s_lblSplitVal,   nullptr, 320, chkSplitY + 30, 100, 18, SWP_NOZORDER | SWP_NOACTIVATE);
        SetWindowPos(s_sliderSplit,   nullptr, 20, chkSplitY + 50, 405, 26, SWP_NOZORDER | SWP_NOACTIVATE);
    }

    const int closeBtnY = splitActive ? (chkSplitY + 88) : (chkSplitY + 34);
    SetWindowPos(s_btnClose,    nullptr, 24, closeBtnY, 396, 36, SWP_NOZORDER | SWP_NOACTIVATE);

    int targetClientH = closeBtnY + 54;
    RECT rc = { 0, 0, 460, targetClientH };
    AdjustWindowRectEx(&rc, GetWindowLongW(s_hwnd, GWL_STYLE), FALSE, GetWindowLongW(s_hwnd, GWL_EXSTYLE));
    int winW = rc.right - rc.left;
    int winH = rc.bottom - rc.top;

    SetWindowPos(s_hwnd, nullptr, 0, 0, winW, winH, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    InvalidateRect(s_hwnd, nullptr, TRUE);
}

void SettingsWindow::UpdateLiveLabels()
{
    wchar_t buf[32];

    int intPos = static_cast<int>(SendMessageW(s_sliderIntensity, TBM_GETPOS, 0, 0));
    swprintf_s(buf, L"%.2fx", intPos / 100.0f);
    SetWindowTextW(s_lblIntensityVal, buf);

    int boostPos = static_cast<int>(SendMessageW(s_sliderBoost, TBM_GETPOS, 0, 0));
    float boostVal = boostPos / 100.0f;
    if (boostPos <= 100)
        swprintf_s(buf, L"1.00x (Normal)");
    else if (boostPos >= 250)
        swprintf_s(buf, L"2.50x (Maks)");
    else
        swprintf_s(buf, L"%.2fx", boostVal);
    SetWindowTextW(s_lblBoostVal, buf);

    int structPos = static_cast<int>(SendMessageW(s_sliderStructure, TBM_GETPOS, 0, 0));
    swprintf_s(buf, L"%.2fx", structPos / 100.0f);
    SetWindowTextW(s_lblStructureVal, buf);

    int tonePos = static_cast<int>(SendMessageW(s_sliderTone, TBM_GETPOS, 0, 0));
    swprintf_s(buf, L"%.2fx", tonePos / 100.0f);
    SetWindowTextW(s_lblToneVal, buf);

    int skinPos = static_cast<int>(SendMessageW(s_sliderSkin, TBM_GETPOS, 0, 0));
    float skinVal = (skinPos / 100.0f) - 1.0f;
    if (skinVal <= -0.99f)
    {
        SetWindowTextW(s_lblSkinVal, L"Otomatik");
    }
    else
    {
        swprintf_s(buf, L"%.2fx", skinVal);
        SetWindowTextW(s_lblSkinVal, buf);
    }

    int scalePos = static_cast<int>(SendMessageW(s_sliderResScale, TBM_GETPOS, 0, 0));
    wchar_t scaleBuf[48];
    if (scalePos >= 100)
        swprintf_s(scaleBuf, L"%%100 (Kalite)");
    else if (scalePos >= 85)
        swprintf_s(scaleBuf, L"%%%d (Dengeli)", scalePos);
    else if (scalePos >= 70)
        swprintf_s(scaleBuf, L"%%%d (Performans)", scalePos);
    else
        swprintf_s(scaleBuf, L"%%%d (Ultra Perf)", scalePos);
    SetWindowTextW(s_lblResScaleVal, scaleBuf);

    int passPos = static_cast<int>(SendMessageW(s_sliderPassCount, TBM_GETPOS, 0, 0));
    wchar_t passBuf[64];
    if (passPos <= 1)
        swprintf_s(passBuf, L"1 (Kapalı)");
    else
        swprintf_s(passBuf, L"%d geçiş (~%dx maliyet)", passPos, passPos);
    SetWindowTextW(s_lblPassCountVal, passBuf);

    int falloffPos = static_cast<int>(SendMessageW(s_sliderPassFalloff, TBM_GETPOS, 0, 0));
    wchar_t falloffBuf[64];
    if (falloffPos >= 100)
        swprintf_s(falloffBuf, L"1.00x (Tam)");
    else
        swprintf_s(falloffBuf, L"%.2fx", falloffPos / 100.0f);
    SetWindowTextW(s_lblPassFalloffVal, falloffBuf);

    int splitPos = static_cast<int>(SendMessageW(s_sliderSplit, TBM_GETPOS, 0, 0));
    wchar_t splitBuf[32];
    if (splitPos == 50)
        swprintf_s(splitBuf, L"%%50 (Tam Orta)");
    else
        swprintf_s(splitBuf, L"%%%d", splitPos);
    SetWindowTextW(s_lblSplitVal, splitBuf);
}

void SettingsWindow::UpdateControlValues()
{
    const auto& cfg = ConfigManager::Get().Config();

    SendMessageW(s_comboStyle, CB_SETCURSEL, cfg.style, 0);
    SendMessageW(s_comboPreset, CB_SETCURSEL, cfg.preset, 0);

    // Intensity: 0.0 - 1.0 -> 0 - 100
    // Ust sinir 1.0: piksel golgelendiricide zaten saturate(g_intensity) var,
    // 1.0 ustu her deger shader tarafinda ayni sonucu veriyordu (bkz. Renderer.cpp:80,105).
    int intPos = static_cast<int>(cfg.intensity * 100.0f + 0.5f);
    if (intPos < 0)   intPos = 0;
    if (intPos > 100) intPos = 100;
    SendMessageW(s_sliderIntensity, TBM_SETPOS, TRUE, intPos);

    // Boost: 1.0 - 2.5 -> 100 - 250
    int boostPos = static_cast<int>(cfg.boostFactor * 100.0f + 0.5f);
    if (boostPos < 100) boostPos = 100;
    if (boostPos > 250) boostPos = 250;
    SendMessageW(s_sliderBoost, TBM_SETPOS, TRUE, boostPos);

    // Local Structure: 0.0 - 2.0 -> 0 - 200
    int structPos = static_cast<int>(cfg.localStructure * 100.0f + 0.5f);
    SendMessageW(s_sliderStructure, TBM_SETPOS, TRUE, structPos);

    // Local Tone: 0.0 - 2.0 -> 0 - 200
    int tonePos = static_cast<int>(cfg.localTone * 100.0f + 0.5f);
    SendMessageW(s_sliderTone, TBM_SETPOS, TRUE, tonePos);

    // Skin Structure: -1.0 - 2.0 -> 0 - 300
    int skinPos = (cfg.skinStructure <= -0.99f) ? 0 : static_cast<int>((cfg.skinStructure + 1.0f) * 100.0f + 0.5f);
    SendMessageW(s_sliderSkin, TBM_SETPOS, TRUE, skinPos);

    // Resolution Scale: 50 - 100
    SendMessageW(s_sliderResScale, TBM_SETPOS, TRUE, cfg.resolutionScale);
    SendMessageW(s_sliderPassCount, TBM_SETPOS, TRUE, cfg.passCount);
    SendMessageW(s_sliderPassFalloff, TBM_SETPOS, TRUE, static_cast<int>(cfg.passFalloff * 100.0f + 0.5f));

    Button_SetCheck(s_chkAutoMask, cfg.useAutoMask ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(s_chkOpticalFlow,  cfg.opticalFlow ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(s_chkSplitScreen, cfg.splitScreen ? BST_CHECKED : BST_UNCHECKED);

    int splitSliderPos = static_cast<int>(cfg.splitPos * 100.0f + 0.5f);
    if (splitSliderPos < 0)   splitSliderPos = 0;
    if (splitSliderPos > 100) splitSliderPos = 100;
    SendMessageW(s_sliderSplit, TBM_SETPOS, TRUE, splitSliderPos);

    UpdateLiveLabels();
    UpdateLayout();
}

void SettingsWindow::OnSettingChanged()
{
    auto& cfg = ConfigManager::Get().Config();

    int selStyle = static_cast<int>(SendMessageW(s_comboStyle, CB_GETCURSEL, 0, 0));
    if (selStyle != CB_ERR) cfg.style = selStyle;

    int selPreset = static_cast<int>(SendMessageW(s_comboPreset, CB_GETCURSEL, 0, 0));
    if (selPreset != CB_ERR) cfg.preset = selPreset;

    int intPos = static_cast<int>(SendMessageW(s_sliderIntensity, TBM_GETPOS, 0, 0));
    cfg.intensity = intPos / 100.0f;

    int boostPos = static_cast<int>(SendMessageW(s_sliderBoost, TBM_GETPOS, 0, 0));
    cfg.boostFactor = boostPos / 100.0f;
    if (cfg.boostFactor < 1.0f) cfg.boostFactor = 1.0f;
    if (cfg.boostFactor > 2.5f) cfg.boostFactor = 2.5f;

    int structPos = static_cast<int>(SendMessageW(s_sliderStructure, TBM_GETPOS, 0, 0));
    cfg.localStructure = structPos / 100.0f;

    int tonePos = static_cast<int>(SendMessageW(s_sliderTone, TBM_GETPOS, 0, 0));
    cfg.localTone = tonePos / 100.0f;

    int skinPos = static_cast<int>(SendMessageW(s_sliderSkin, TBM_GETPOS, 0, 0));
    cfg.skinStructure = (skinPos / 100.0f) - 1.0f;
    if (cfg.skinStructure <= -0.99f)
    {
        cfg.skinStructure = -1.0f;
    }

    int scalePos = static_cast<int>(SendMessageW(s_sliderResScale, TBM_GETPOS, 0, 0));
    cfg.resolutionScale = scalePos;

    int passPos = static_cast<int>(SendMessageW(s_sliderPassCount, TBM_GETPOS, 0, 0));
    cfg.passCount = passPos;
    if (cfg.passCount < 1) cfg.passCount = 1;
    if (cfg.passCount > 4) cfg.passCount = 4;

    int falloffPos = static_cast<int>(SendMessageW(s_sliderPassFalloff, TBM_GETPOS, 0, 0));
    cfg.passFalloff = falloffPos / 100.0f;
    if (cfg.passFalloff < 0.25f) cfg.passFalloff = 0.25f;
    if (cfg.passFalloff > 1.0f)  cfg.passFalloff = 1.0f;

    cfg.useAutoMask = (Button_GetCheck(s_chkAutoMask) == BST_CHECKED);
    cfg.opticalFlow        = (Button_GetCheck(s_chkOpticalFlow)  == BST_CHECKED);
    cfg.splitScreen = (Button_GetCheck(s_chkSplitScreen) == BST_CHECKED);

    int splitPos = static_cast<int>(SendMessageW(s_sliderSplit, TBM_GETPOS, 0, 0));
    cfg.splitPos = splitPos / 100.0f;
    if (cfg.splitPos < 0.0f) cfg.splitPos = 0.0f;
    if (cfg.splitPos > 1.0f) cfg.splitPos = 1.0f;

    UpdateLiveLabels();

    ConfigManager::Get().Save();

    if (s_callback)
    {
        s_callback(cfg);
    }
}

LRESULT CALLBACK SettingsWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
        CreateControls(hwnd);
        UpdateControlValues();
        return 0;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT client;
        GetClientRect(hwnd, &client);

        FillRect(hdc, &client, s_brBg);

        // Header Accent Line (Neon Green with glow)
        HPEN hPenGlow = CreatePen(PS_SOLID, 3, RGB(0, 100, 25));
        HGDIOBJ oldPen = SelectObject(hdc, hPenGlow);
        MoveToEx(hdc, 24, 66, nullptr);
        LineTo(hdc, client.right - 24, 66);

        HPEN hPen = CreatePen(PS_SOLID, 1, COLOR_NEON_GREEN);
        SelectObject(hdc, hPen);
        MoveToEx(hdc, 24, 66, nullptr);
        LineTo(hdc, client.right - 24, 66);

        SelectObject(hdc, oldPen);
        DeleteObject(hPen);
        DeleteObject(hPenGlow);

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_CTLCOLORSTATIC:
    {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        HWND ctl = reinterpret_cast<HWND>(lParam);
        SetBkMode(hdc, TRANSPARENT);

        if (ctl == s_lblIntensityVal || ctl == s_lblBoostVal || ctl == s_lblStructureVal ||
            ctl == s_lblToneVal || ctl == s_lblSkinVal ||
            ctl == s_lblResScaleVal || ctl == s_lblSplitVal ||
            ctl == s_lblPassCountVal || ctl == s_lblPassFalloffVal)
        {
            SetTextColor(hdc, COLOR_NEON_GREEN);
            return reinterpret_cast<LRESULT>(s_brBg);
        }

        SetTextColor(hdc, COLOR_TEXT_MAIN);
        return reinterpret_cast<LRESULT>(s_brBg);
    }

    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLOREDIT:
    {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetBkMode(hdc, OPAQUE);
        SetBkColor(hdc, COLOR_CARD_BG);
        SetTextColor(hdc, COLOR_TEXT_MAIN);
        return reinterpret_cast<LRESULT>(s_brCard);
    }

    case WM_HSCROLL:
    {
        WORD scrollCode = LOWORD(wParam);
        bool isFinalPosition = (scrollCode == TB_ENDTRACK || scrollCode == TB_THUMBPOSITION);

        UpdateLiveLabels();

        HWND src = reinterpret_cast<HWND>(lParam);
        // Geçiş sayısı da pahalı: her adımda N adet feature handle yeniden kurulur.
        // Falloff pahalı DEĞİL -- yalnızca Evaluate'e geçen bir çarpan, anında uygulanır.
        bool isExpensiveSlider = (src == s_sliderResScale || src == s_sliderPassCount);

        if (!isExpensiveSlider || isFinalPosition)
            OnSettingChanged();
        return 0;
    }

    case WM_COMMAND:
    {
        WORD id = LOWORD(wParam);
        WORD code = HIWORD(wParam);

        if (code == CBN_SELCHANGE)
        {
            OnSettingChanged();
            return 0;
        }

        if (id == IDC_SW_CHK_AUTOMASK || id == IDC_SW_CHK_OPTFLOW
            || id == IDC_SW_CHK_SPLIT)
        {
            if (id == IDC_SW_CHK_SPLIT)
            {
                UpdateLayout();
            }
            OnSettingChanged();
            return 0;
        }

        if (id == IDC_SW_BTN_CLOSE)
        {
            Hide();
            return 0;
        }
        break;
    }

    case WM_DRAWITEM:
    {
        auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
        if (!dis) break;

        if (dis->CtlID == IDC_SW_BTN_CLOSE)
        {
            bool isPressed = (dis->itemState & ODS_SELECTED);

            COLORREF bg      = isPressed ? RGB(0, 185, 45) : RGB(0, 235, 60);
            COLORREF border  = bg;
            COLORREF textCol = RGB(8, 14, 18);

            HPEN hPen = CreatePen(PS_SOLID, 1, border);
            HBRUSH hBr = CreateSolidBrush(bg);
            HGDIOBJ oldPen = SelectObject(dis->hDC, hPen);
            HGDIOBJ oldBr = SelectObject(dis->hDC, hBr);

            RoundRect(dis->hDC, dis->rcItem.left, dis->rcItem.top, dis->rcItem.right, dis->rcItem.bottom, 8, 8);

            SelectObject(dis->hDC, oldBr);
            SelectObject(dis->hDC, oldPen);
            DeleteObject(hBr);
            DeleteObject(hPen);

            wchar_t btnText[64] = {};
            GetWindowTextW(dis->hwndItem, btnText, _countof(btnText));

            SetBkMode(dis->hDC, TRANSPARENT);
            SetTextColor(dis->hDC, textCol);
            SelectObject(dis->hDC, s_fontBold);
            DrawTextW(dis->hDC, btnText, -1, &dis->rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            return TRUE;
        }
        break;
    }

    case WM_CLOSE:
        Hide();
        return 0;

    case WM_DESTROY:
        s_hwnd = nullptr;
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
