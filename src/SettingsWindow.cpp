#include "SettingsWindow.h"
#include "resource.h"
#include <commctrl.h>
#include <cstdio>

#pragma comment(lib, "comctl32.lib")

static constexpr COLORREF COLOR_DARK_BG    = RGB(13, 17, 23);
static constexpr COLORREF COLOR_CARD_BG    = RGB(22, 28, 38);
static constexpr COLORREF COLOR_BORDER     = RGB(38, 48, 65);
static constexpr COLORREF COLOR_NEON_GREEN = RGB(0, 240, 55);
static constexpr COLORREF COLOR_NEON_DARK  = RGB(0, 180, 40);
static constexpr COLORREF COLOR_TEXT_MAIN  = RGB(240, 246, 252);
static constexpr COLORREF COLOR_TEXT_DIM   = RGB(139, 148, 158);

#define IDC_SW_STYLE          501
#define IDC_SW_PRESET         502
#define IDC_SW_SLIDER_INTENSE 503
#define IDC_SW_SLIDER_STRUCT  504
#define IDC_SW_SLIDER_TONE    505
#define IDC_SW_SLIDER_SKIN    506
#define IDC_SW_SLIDER_SHARPNESS 512
#define IDC_SW_SLIDER_RESSCALE 507
#define IDC_SW_CHK_AUTOMASK   508
#define IDC_SW_CHK_STABILIZER 511
#define IDC_SW_BTN_REBIND     509
#define IDC_SW_BTN_CLOSE      510

HWND                             SettingsWindow::s_hwnd             = nullptr;
HINSTANCE                        SettingsWindow::s_hInstance        = nullptr;
bool                             SettingsWindow::s_rebindingKey     = false;
HHOOK                            SettingsWindow::s_rebindHook       = nullptr;
SettingsWindow::ConfigChangedCallback SettingsWindow::s_callback   = nullptr;

HWND SettingsWindow::s_comboStyle        = nullptr;
HWND SettingsWindow::s_comboPreset       = nullptr;
HWND SettingsWindow::s_sliderIntensity   = nullptr;
HWND SettingsWindow::s_lblIntensityVal   = nullptr;
HWND SettingsWindow::s_sliderStructure   = nullptr;
HWND SettingsWindow::s_lblStructureVal   = nullptr;
HWND SettingsWindow::s_sliderTone        = nullptr;
HWND SettingsWindow::s_lblToneVal        = nullptr;
HWND SettingsWindow::s_sliderSkin        = nullptr;
HWND SettingsWindow::s_lblSkinVal        = nullptr;
HWND SettingsWindow::s_sliderSharpness   = nullptr;
HWND SettingsWindow::s_lblSharpnessVal   = nullptr;
HWND SettingsWindow::s_sliderResScale    = nullptr;
HWND SettingsWindow::s_lblResScaleVal    = nullptr;
HWND SettingsWindow::s_chkAutoMask       = nullptr;
HWND SettingsWindow::s_chkStabilizer     = nullptr;
HWND SettingsWindow::s_lblHotkey         = nullptr;
HWND SettingsWindow::s_btnRebind         = nullptr;
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

    s_fontTitle  = CreateFontW(-18, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    s_fontBold   = CreateFontW(-14, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    s_fontNormal = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    s_fontSmall  = CreateFontW(-11, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

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
}

void SettingsWindow::Show(HWND parent)
{
    if (!s_hwnd)
    {
        int w = 460;
        int h = 715;
        int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
        int y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;

        s_hwnd = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
            L"VLSS5_SettingsWindowClass",
            L"DLSS 5 Nöral Yapılandırma",
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

void SettingsWindow::CreateControls(HWND hwnd)
{
    // --- Header ---
    HWND lblTitle = CreateWindowW(L"STATIC", L"DLSS 5 NÖRAL AYARLAR",
        WS_CHILD | WS_VISIBLE, 24, 16, 400, 24, hwnd, nullptr, nullptr, nullptr);
    SetFont(lblTitle, s_fontTitle);

    HWND lblSubtitle = CreateWindowW(L"STATIC", L"NVIDIA NGX Feature 18 Canlı Yapılandırma Paneli",
        WS_CHILD | WS_VISIBLE, 24, 40, 400, 18, hwnd, nullptr, nullptr, nullptr);
    SetFont(lblSubtitle, s_fontSmall);

    int y = 72;

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
    SendMessageW(s_comboStyle, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Standard (Keskin)"));
    SendMessageW(s_comboStyle, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Natural (Doğal)"));
    SendMessageW(s_comboStyle, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Cinematic (Film)"));

    s_comboPreset = CreateWindowW(L"COMBOBOX", nullptr,
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        230, y, 190, 140, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SW_PRESET)), s_hInstance, nullptr);
    SetFont(s_comboPreset, s_fontNormal);
    SendMessageW(s_comboPreset, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Default (Önerilen)"));
    SendMessageW(s_comboPreset, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Preset 1"));
    SendMessageW(s_comboPreset, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Preset 2"));
    SendMessageW(s_comboPreset, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Preset 3"));

    y += 40;

    // --- Sliders Helper Macro ---
    auto createSliderRow = [&](const wchar_t* title, HWND& slider, HWND& valLabel, INT_PTR id, int minV, int maxV) {
        HWND lbl = CreateWindowW(L"STATIC", title,
            WS_CHILD | WS_VISIBLE, 24, y, 280, 18, hwnd, nullptr, nullptr, nullptr);
        SetFont(lbl, s_fontBold);

        valLabel = CreateWindowW(L"STATIC", L"1.00x",
            WS_CHILD | WS_VISIBLE | SS_RIGHT, 320, y, 100, 18, hwnd, nullptr, nullptr, nullptr);
        SetFont(valLabel, s_fontBold);

        y += 20;

        slider = CreateWindowExW(0, TRACKBAR_CLASSW, nullptr,
            WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_AUTOTICKS,
            20, y, 405, 30, hwnd, reinterpret_cast<HMENU>(id), s_hInstance, nullptr);
        SendMessageW(slider, TBM_SETRANGE, TRUE, MAKELPARAM(minV, maxV));
        SendMessageW(slider, TBM_SETTICFREQ, 25, 0);

        y += 36;
    };

    createSliderRow(L"Nöral Şiddet (Intensity):", s_sliderIntensity, s_lblIntensityVal, IDC_SW_SLIDER_INTENSE, 0, 200);
    createSliderRow(L"Yüzey Detayı (Local Structure):", s_sliderStructure, s_lblStructureVal, IDC_SW_SLIDER_STRUCT, 0, 200);
    createSliderRow(L"Mikro Kontrast (Local Tone):", s_sliderTone, s_lblToneVal, IDC_SW_SLIDER_TONE, 0, 200);
    createSliderRow(L"Ten Doku Ayarı (Skin Structure):", s_sliderSkin, s_lblSkinVal, IDC_SW_SLIDER_SKIN, 0, 300);
    createSliderRow(L"Keskinleştirme (RCAS Sharpness):", s_sliderSharpness, s_lblSharpnessVal, IDC_SW_SLIDER_SHARPNESS, 0, 100);
    SendMessageW(s_sliderSharpness, TBM_SETTICFREQ, 10, 0);
    createSliderRow(L"Model Çözünürlüğü (Performans):", s_sliderResScale, s_lblResScaleVal, IDC_SW_SLIDER_RESSCALE, 50, 100);
    SendMessageW(s_sliderResScale, TBM_SETTICFREQ, 5, 0);

    // --- Checkbox: Auto Skin Mask ---
    s_chkAutoMask = CreateWindowW(L"BUTTON", L" Otomatik Ten Tespiti (Auto Skin Mask)",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        24, y, 396, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SW_CHK_AUTOMASK)), s_hInstance, nullptr);
    SetFont(s_chkAutoMask, s_fontNormal);

    y += 28;

    // --- Checkbox: Optical Flow Motion Tracking ---
    s_chkStabilizer = CreateWindowW(L"BUTTON", L" Optik Akış & Hareket Takibi (Optical Flow)",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        24, y, 396, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SW_CHK_STABILIZER)), s_hInstance, nullptr);
    SetFont(s_chkStabilizer, s_fontNormal);

    y += 34;

    // --- Hotkey Rebind Row ---
    HWND lblKeyTitle = CreateWindowW(L"STATIC", L"Menü Kısayol Tuşu:",
        WS_CHILD | WS_VISIBLE, 24, y + 4, 150, 22, hwnd, nullptr, nullptr, nullptr);
    SetFont(lblKeyTitle, s_fontBold);

    s_lblHotkey = CreateWindowW(L"STATIC", L"INSERT",
        WS_CHILD | WS_VISIBLE | SS_CENTER, 175, y, 120, 28, hwnd, nullptr, nullptr, nullptr);
    SetFont(s_lblHotkey, s_fontBold);

    s_btnRebind = CreateWindowW(L"BUTTON", L"Değiştir",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 305, y, 115, 28, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SW_BTN_REBIND)), s_hInstance, nullptr);
    SetFont(s_btnRebind, s_fontNormal);

    y += 44;

    // --- Close Button ---
    s_btnClose = CreateWindowW(L"BUTTON", L"KAPAT / UYGULA",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        24, y, 396, 36, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SW_BTN_CLOSE)), s_hInstance, nullptr);
    SetFont(s_btnClose, s_fontBold);
}

void SettingsWindow::UpdateControlValues()
{
    const auto& cfg = ConfigManager::Get().Config();

    SendMessageW(s_comboStyle, CB_SETCURSEL, cfg.style, 0);
    SendMessageW(s_comboPreset, CB_SETCURSEL, cfg.preset, 0);

    // Intensity: 0.0 - 2.0 -> 0 - 200
    int intPos = static_cast<int>(cfg.intensity * 100.0f + 0.5f);
    SendMessageW(s_sliderIntensity, TBM_SETPOS, TRUE, intPos);
    wchar_t buf[32];
    swprintf_s(buf, L"%.2fx", cfg.intensity);
    SetWindowTextW(s_lblIntensityVal, buf);

    // Local Structure: 0.0 - 2.0 -> 0 - 200
    int structPos = static_cast<int>(cfg.localStructure * 100.0f + 0.5f);
    SendMessageW(s_sliderStructure, TBM_SETPOS, TRUE, structPos);
    swprintf_s(buf, L"%.2fx", cfg.localStructure);
    SetWindowTextW(s_lblStructureVal, buf);

    // Local Tone: 0.0 - 2.0 -> 0 - 200
    int tonePos = static_cast<int>(cfg.localTone * 100.0f + 0.5f);
    SendMessageW(s_sliderTone, TBM_SETPOS, TRUE, tonePos);
    swprintf_s(buf, L"%.2fx", cfg.localTone);
    SetWindowTextW(s_lblToneVal, buf);

    // Skin Structure: -1.0 - 2.0 -> 0 - 300
    int skinPos = static_cast<int>((cfg.skinStructure + 1.0f) * 100.0f + 0.5f);
    SendMessageW(s_sliderSkin, TBM_SETPOS, TRUE, skinPos);
    if (cfg.skinStructure <= -0.99f)
        SetWindowTextW(s_lblSkinVal, L"Otomatik");
    else
    {
        swprintf_s(buf, L"%.2fx", cfg.skinStructure);
        SetWindowTextW(s_lblSkinVal, buf);
    }

    // Sharpness: 0.0 - 1.0 -> 0 - 100
    int sharpPos = static_cast<int>(cfg.sharpness * 100.0f + 0.5f);
    SendMessageW(s_sliderSharpness, TBM_SETPOS, TRUE, sharpPos);
    wchar_t sharpBuf[32];
    if (cfg.sharpness <= 0.001f)
        swprintf_s(sharpBuf, L"Kapalı");
    else
        swprintf_s(sharpBuf, L"%%%d", sharpPos);
    SetWindowTextW(s_lblSharpnessVal, sharpBuf);

    // Resolution Scale: 50 - 100
    SendMessageW(s_sliderResScale, TBM_SETPOS, TRUE, cfg.resolutionScale);
    wchar_t scaleBuf[48];
    if (cfg.resolutionScale >= 100)
        swprintf_s(scaleBuf, L"%%100 (Kalite)");
    else if (cfg.resolutionScale >= 85)
        swprintf_s(scaleBuf, L"%%%d (Dengeli)", cfg.resolutionScale);
    else if (cfg.resolutionScale >= 70)
        swprintf_s(scaleBuf, L"%%%d (Performans)", cfg.resolutionScale);
    else
        swprintf_s(scaleBuf, L"%%%d (Ultra Perf)", cfg.resolutionScale);
    SetWindowTextW(s_lblResScaleVal, scaleBuf);

    Button_SetCheck(s_chkAutoMask, cfg.useAutoMask ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(s_chkStabilizer, cfg.opticalFlow ? BST_CHECKED : BST_UNCHECKED);
    SetWindowTextW(s_lblHotkey, cfg.FormatHotkey().c_str());
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
    wchar_t buf[32];
    swprintf_s(buf, L"%.2fx", cfg.intensity);
    SetWindowTextW(s_lblIntensityVal, buf);

    int structPos = static_cast<int>(SendMessageW(s_sliderStructure, TBM_GETPOS, 0, 0));
    cfg.localStructure = structPos / 100.0f;
    swprintf_s(buf, L"%.2fx", cfg.localStructure);
    SetWindowTextW(s_lblStructureVal, buf);

    int tonePos = static_cast<int>(SendMessageW(s_sliderTone, TBM_GETPOS, 0, 0));
    cfg.localTone = tonePos / 100.0f;
    swprintf_s(buf, L"%.2fx", cfg.localTone);
    SetWindowTextW(s_lblToneVal, buf);

    int skinPos = static_cast<int>(SendMessageW(s_sliderSkin, TBM_GETPOS, 0, 0));
    cfg.skinStructure = (skinPos / 100.0f) - 1.0f;
    if (cfg.skinStructure <= -0.99f)
    {
        cfg.skinStructure = -1.0f;
        SetWindowTextW(s_lblSkinVal, L"Otomatik");
    }
    else
    {
        swprintf_s(buf, L"%.2fx", cfg.skinStructure);
        SetWindowTextW(s_lblSkinVal, buf);
    }

    int sharpPos = static_cast<int>(SendMessageW(s_sliderSharpness, TBM_GETPOS, 0, 0));
    cfg.sharpness = sharpPos / 100.0f;
    wchar_t sharpBuf[32];
    if (cfg.sharpness <= 0.001f)
        swprintf_s(sharpBuf, L"Kapalı");
    else
        swprintf_s(sharpBuf, L"%%%d", sharpPos);
    SetWindowTextW(s_lblSharpnessVal, sharpBuf);

    int scalePos = static_cast<int>(SendMessageW(s_sliderResScale, TBM_GETPOS, 0, 0));
    cfg.resolutionScale = scalePos;
    wchar_t scaleBuf[48];
    if (cfg.resolutionScale >= 100)
        swprintf_s(scaleBuf, L"%%100 (Kalite)");
    else if (cfg.resolutionScale >= 85)
        swprintf_s(scaleBuf, L"%%%d (Dengeli)", cfg.resolutionScale);
    else if (cfg.resolutionScale >= 70)
        swprintf_s(scaleBuf, L"%%%d (Performans)", cfg.resolutionScale);
    else
        swprintf_s(scaleBuf, L"%%%d (Ultra Perf)", cfg.resolutionScale);
    SetWindowTextW(s_lblResScaleVal, scaleBuf);

    cfg.useAutoMask = (Button_GetCheck(s_chkAutoMask) == BST_CHECKED);
    cfg.opticalFlow = (Button_GetCheck(s_chkStabilizer) == BST_CHECKED);


    ConfigManager::Get().Save();

    if (s_callback)
    {
        s_callback(cfg);
    }
}

void SettingsWindow::StartKeybindCapture()
{
    if (s_rebindingKey) return;
    s_rebindingKey = true;
    SetWindowTextW(s_btnRebind, L"Tuşa Basın...");

    s_rebindHook = SetWindowsHookExW(
        WH_KEYBOARD_LL,
        RebindKeyboardProc,
        s_hInstance,
        0);
}

void SettingsWindow::EndKeybindCapture()
{
    if (s_rebindHook)
    {
        UnhookWindowsHookEx(s_rebindHook);
        s_rebindHook = nullptr;
    }
    s_rebindingKey = false;
    SetWindowTextW(s_btnRebind, L"Değiştir");
    SetWindowTextW(s_lblHotkey, ConfigManager::Get().Config().FormatHotkey().c_str());
}

LRESULT CALLBACK SettingsWindow::RebindKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode >= HC_ACTION && (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN))
    {
        auto* kbd = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
        UINT vk = kbd->vkCode;

        // Ignore pure modifier keys as standalone trigger
        if (vk != VK_LCONTROL && vk != VK_RCONTROL &&
            vk != VK_LMENU    && vk != VK_RMENU    &&
            vk != VK_LSHIFT   && vk != VK_RSHIFT   &&
            vk != VK_LWIN     && vk != VK_RWIN)
        {
            UINT mod = 0;
            if (GetAsyncKeyState(VK_CONTROL) & 0x8000) mod |= MOD_CONTROL;
            if (GetAsyncKeyState(VK_MENU)    & 0x8000) mod |= MOD_ALT;
            if (GetAsyncKeyState(VK_SHIFT)   & 0x8000) mod |= MOD_SHIFT;

            ConfigManager::Get().Config().settingsVk  = vk;
            ConfigManager::Get().Config().settingsMod = mod;
            ConfigManager::Get().Save();

            EndKeybindCapture();
            return 1; // consume key
        }
    }
    return CallNextHookEx(s_rebindHook, nCode, wParam, lParam);
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

        // Header Accent Line (Neon Green)
        HPEN hPen = CreatePen(PS_SOLID, 2, COLOR_NEON_GREEN);
        HGDIOBJ oldPen = SelectObject(hdc, hPen);
        MoveToEx(hdc, 0, 62, nullptr);
        LineTo(hdc, client.right, 62);
        SelectObject(hdc, oldPen);
        DeleteObject(hPen);

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_CTLCOLORSTATIC:
    {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        HWND ctl = reinterpret_cast<HWND>(lParam);
        SetBkMode(hdc, TRANSPARENT);

        if (ctl == s_lblIntensityVal || ctl == s_lblStructureVal ||
            ctl == s_lblToneVal || ctl == s_lblSkinVal ||
            ctl == s_lblResScaleVal || ctl == s_lblHotkey)
        {
            SetTextColor(hdc, COLOR_NEON_GREEN);
            return reinterpret_cast<LRESULT>(s_brCard);
        }

        SetTextColor(hdc, COLOR_TEXT_MAIN);
        return reinterpret_cast<LRESULT>(s_brBg);
    }

    case WM_HSCROLL:
        OnSettingChanged();
        return 0;

    case WM_COMMAND:
    {
        WORD id = LOWORD(wParam);
        WORD code = HIWORD(wParam);

        if (code == CBN_SELCHANGE)
        {
            OnSettingChanged();
            return 0;
        }

        if (id == IDC_SW_CHK_AUTOMASK || id == IDC_SW_CHK_STABILIZER)
        {
            OnSettingChanged();
            return 0;
        }

        if (id == IDC_SW_BTN_REBIND)
        {
            if (!s_rebindingKey) StartKeybindCapture();
            else EndKeybindCapture();
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

        if (dis->CtlID == IDC_SW_BTN_REBIND || dis->CtlID == IDC_SW_BTN_CLOSE)
        {
            bool isClose = (dis->CtlID == IDC_SW_BTN_CLOSE);
            bool isPressed = (dis->itemState & ODS_SELECTED);

            COLORREF bg = isClose
                ? (isPressed ? COLOR_NEON_DARK : COLOR_NEON_GREEN)
                : (isPressed ? RGB(35, 45, 60) : COLOR_CARD_BG);

            COLORREF border = isClose ? bg : COLOR_NEON_GREEN;
            COLORREF textCol = isClose ? RGB(10, 16, 20) : COLOR_NEON_GREEN;

            HPEN hPen = CreatePen(PS_SOLID, 1, border);
            HBRUSH hBr = CreateSolidBrush(bg);
            HGDIOBJ oldPen = SelectObject(dis->hDC, hPen);
            HGDIOBJ oldBr = SelectObject(dis->hDC, hBr);

            RoundRect(dis->hDC, dis->rcItem.left, dis->rcItem.top, dis->rcItem.right, dis->rcItem.bottom, 6, 6);

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
        EndKeybindCapture();
        s_hwnd = nullptr;
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
