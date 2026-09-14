#include "RtssWindow.h"
#include <commctrl.h>
#include <uxtheme.h>
#include <shobjidl.h>

static constexpr COLORREF COLOR_DARK_BG    = RGB(15, 19, 26);
static constexpr COLORREF COLOR_CARD_BG    = RGB(22, 28, 38);
static constexpr COLORREF COLOR_BORDER     = RGB(38, 48, 65);
static constexpr COLORREF COLOR_NEON_GREEN = RGB(0, 240, 55);
static constexpr COLORREF COLOR_TEXT_MAIN  = RGB(242, 247, 252);
static constexpr COLORREF COLOR_TEXT_TITLE = RGB(220, 245, 230);
static constexpr COLORREF COLOR_TEXT_DIM   = RGB(139, 148, 158);

#define IDC_RW_BTN_CLOSE        601
#define IDC_RW_BTN_DIR          602
#define IDC_RW_COMBO_PROF       603
#define IDC_RW_BTN_REFRESH      604
#define IDC_RW_BTN_DELETE       605

HWND RtssWindow::s_hwnd = nullptr;
HINSTANCE RtssWindow::s_hInstance = nullptr;
HWND RtssWindow::s_lblRtssDirTitle = nullptr;
HWND RtssWindow::s_btnRtssDir = nullptr;
HWND RtssWindow::s_lblRtssProfTitle = nullptr;
HWND RtssWindow::s_comboRtssProf = nullptr;
HWND RtssWindow::s_btnRefresh = nullptr;
HWND RtssWindow::s_btnDelete = nullptr;
HWND RtssWindow::s_btnClose = nullptr;

HFONT RtssWindow::s_fontTitle = nullptr;
HFONT RtssWindow::s_fontNormal = nullptr;
HFONT RtssWindow::s_fontBold = nullptr;
HBRUSH RtssWindow::s_brBg = nullptr;
HBRUSH RtssWindow::s_brCard = nullptr;
HBRUSH RtssWindow::s_brBorder = nullptr;

static void DrawModernPanel2(HDC hdc, RECT rc, COLORREF bg, COLORREF border, int radius)
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

void RtssWindow::Initialize(HINSTANCE hInstance)
{
    s_hInstance = hInstance;
    
    WNDCLASSEXW wc = { sizeof(WNDCLASSEXW) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"RtssWindowClass";
    RegisterClassExW(&wc);

    s_brBg = CreateSolidBrush(COLOR_DARK_BG);
    s_brCard = CreateSolidBrush(COLOR_CARD_BG);
    s_brBorder = CreateSolidBrush(COLOR_BORDER);

    s_fontTitle = CreateFontW(24, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    s_fontNormal = CreateFontW(14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    s_fontBold = CreateFontW(14, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
}

void RtssWindow::Show(HWND parent)
{
    if (s_hwnd)
    {
        ShowWindow(s_hwnd, SW_SHOW);
        SetForegroundWindow(s_hwnd);
        return;
    }

    int width = 500;
    int height = 300;
    
    RECT rcParent;
    if (parent) GetWindowRect(parent, &rcParent);
    else { rcParent.left = 100; rcParent.top = 100; rcParent.right = 100 + width; rcParent.bottom = 100 + height; }

    int cx = rcParent.left + (rcParent.right - rcParent.left - width) / 2;
    int cy = rcParent.top + (rcParent.bottom - rcParent.top - height) / 2;

    s_hwnd = CreateWindowExW(WS_EX_TOPMOST, L"RtssWindowClass", L"RTSS Ayarlari",
        WS_POPUP, cx, cy, width, height, parent, nullptr, s_hInstance, nullptr);
    
    CreateControls(s_hwnd);
    PopulateRTSSList();
    
    ShowWindow(s_hwnd, SW_SHOW);
    UpdateWindow(s_hwnd);
}

void RtssWindow::Hide()
{
    if (s_hwnd) ShowWindow(s_hwnd, SW_HIDE);
}

bool RtssWindow::IsOpen()
{
    return s_hwnd && IsWindowVisible(s_hwnd);
}

void RtssWindow::CreateControls(HWND hwnd)
{
    auto SF = [](HWND h, HFONT f) { SendMessageW(h, WM_SETFONT, (WPARAM)f, FALSE); };

    s_btnClose = CreateWindowW(L"BUTTON", L"X",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        450, 20, 30, 30, hwnd, (HMENU)IDC_RW_BTN_CLOSE, nullptr, nullptr);
    SF(s_btnClose, s_fontBold);

    s_lblRtssDirTitle = CreateWindowW(L"STATIC", L"RTSS Klasörü:",
        WS_CHILD | WS_VISIBLE, 30, 70, 150, 20, hwnd, nullptr, nullptr, nullptr);
    SF(s_lblRtssDirTitle, s_fontBold);

    s_btnRtssDir = CreateWindowW(L"BUTTON", L"Klasör Seç",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        180, 65, 120, 30, hwnd, (HMENU)IDC_RW_BTN_DIR, nullptr, nullptr);
    SF(s_btnRtssDir, s_fontNormal);

    s_lblRtssProfTitle = CreateWindowW(L"STATIC", L"Profiller:",
        WS_CHILD | WS_VISIBLE, 30, 120, 150, 20, hwnd, nullptr, nullptr, nullptr);
    SF(s_lblRtssProfTitle, s_fontBold);

    s_comboRtssProf = CreateWindowExW(0, L"COMBOBOX", nullptr,
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWNLIST,
        180, 115, 200, 150, hwnd, (HMENU)IDC_RW_COMBO_PROF, nullptr, nullptr);
    SF(s_comboRtssProf, s_fontNormal);
    SetWindowTheme(s_comboRtssProf, L"DarkMode_Explorer", nullptr);

    s_btnRefresh = CreateWindowW(L"BUTTON", L"Yenile",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        390, 115, 80, 30, hwnd, (HMENU)IDC_RW_BTN_REFRESH, nullptr, nullptr);
    SF(s_btnRefresh, s_fontNormal);

    s_btnDelete = CreateWindowW(L"BUTTON", L"Profili Sil",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        180, 160, 290, 40, hwnd, (HMENU)IDC_RW_BTN_DELETE, nullptr, nullptr);
    SF(s_btnDelete, s_fontBold);
}

void RtssWindow::PopulateRTSSList()
{
    if (!s_comboRtssProf) return;
    SendMessageW(s_comboRtssProf, CB_RESETCONTENT, 0, 0);
    std::vector<std::wstring> profiles = RTSSManager::Get().GetProfiles();
    for (const auto& prof : profiles)
    {
        SendMessageW(s_comboRtssProf, CB_ADDSTRING, 0, (LPARAM)prof.c_str());
    }
    if (!profiles.empty())
    {
        SendMessageW(s_comboRtssProf, CB_SETCURSEL, 0, 0);
    }
}

LRESULT CALLBACK RtssWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        DrawModernPanel2(hdc, rc, COLOR_DARK_BG, COLOR_NEON_GREEN, 10);
        
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, COLOR_TEXT_TITLE);
        SelectObject(hdc, s_fontTitle);
        rc.top = 20; rc.left = 30;
        DrawTextW(hdc, L"RTSS Ayarları", -1, &rc, DT_SINGLELINE | DT_LEFT | DT_TOP);
        
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
        
        if (dis->CtlID == IDC_RW_BTN_CLOSE) {
            btnBg = isPressed ? RGB(200,50,50) : COLOR_CARD_BG;
            DrawModernPanel2(dis->hDC, dis->rcItem, btnBg, COLOR_BORDER, 6);
            SetBkMode(dis->hDC, TRANSPARENT);
            SetTextColor(dis->hDC, textCol);
            SelectObject(dis->hDC, s_fontBold);
            DrawTextW(dis->hDC, L"X", -1, &dis->rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            return TRUE;
        }

        if (dis->CtlID == IDC_RW_BTN_DELETE) {
            btnBg = isPressed ? RGB(180,40,40) : RGB(220,50,50);
            textCol = RGB(255,255,255);
        } else if (isPressed) {
            btnBg = COLOR_BORDER;
        }

        DrawModernPanel2(dis->hDC, dis->rcItem, btnBg, COLOR_BORDER, 6);
        SetBkMode(dis->hDC, TRANSPARENT);
        SetTextColor(dis->hDC, textCol);
        SelectObject(dis->hDC, (dis->CtlID == IDC_RW_BTN_DELETE) ? s_fontBold : s_fontNormal);

        wchar_t text[64] = {};
        GetWindowTextW(dis->hwndItem, text, 64);
        DrawTextW(dis->hDC, text, -1, &dis->rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return TRUE;
    }
    case WM_COMMAND:
    {
        int ctlId = LOWORD(wParam);
        if (ctlId == IDC_RW_BTN_CLOSE)
        {
            Hide();
            return 0;
        }
        else if (ctlId == IDC_RW_BTN_DIR)
        {
            IFileDialog* pfd = nullptr;
            if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pfd))))
            {
                DWORD dwOptions;
                if (SUCCEEDED(pfd->GetOptions(&dwOptions))) pfd->SetOptions(dwOptions | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
                if (SUCCEEDED(pfd->Show(hwnd)))
                {
                    IShellItem* psi = nullptr;
                    if (SUCCEEDED(pfd->GetResult(&psi)))
                    {
                        PWSTR pszPath = nullptr;
                        if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &pszPath)))
                        {
                            ConfigManager::Get().Config().rtssDirectory = pszPath;
                            ConfigManager::Get().Save();
                            CoTaskMemFree(pszPath);
                            PopulateRTSSList();
                        }
                        psi->Release();
                    }
                }
                pfd->Release();
            }
        }
        else if (ctlId == IDC_RW_BTN_REFRESH)
        {
            PopulateRTSSList();
        }
        else if (ctlId == IDC_RW_BTN_DELETE)
        {
            int sel = (int)SendMessageW(s_comboRtssProf, CB_GETCURSEL, 0, 0);
            if (sel != CB_ERR)
            {
                wchar_t buf[256] = {};
                SendMessageW(s_comboRtssProf, CB_GETLBTEXT, sel, (LPARAM)buf);
                std::wstring cfgPath = RTSSManager::Get().GetProfilesDir() + L"\\" + buf + L".cfg";
                DeleteFileW(cfgPath.c_str());
                RTSSManager::Get().NotifyRTSS();
                PopulateRTSSList();
            }
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
