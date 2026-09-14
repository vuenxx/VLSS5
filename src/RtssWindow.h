#pragma once
#include "Common.h"
#include "ConfigManager.h"
#include "RTSSManager.h"

class RtssWindow
{
public:
    static void Initialize(HINSTANCE hInstance);
    static void Show(HWND parent = nullptr);
    static void Hide();
    static bool IsOpen();
    static HWND GetHwnd() { return s_hwnd; }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static void CreateControls(HWND hwnd);
    static void PopulateRTSSList();

    static HWND s_hwnd;
    static HINSTANCE s_hInstance;

    // Controls
    static HWND s_lblRtssDirTitle;
    static HWND s_btnRtssDir;
    static HWND s_lblRtssProfTitle;
    static HWND s_comboRtssProf;
    static HWND s_btnRefresh;
    static HWND s_btnDelete;
    static HWND s_btnClose;

    // GDI resources
    static HFONT s_fontTitle;
    static HFONT s_fontNormal;
    static HFONT s_fontBold;
    static HBRUSH s_brBg;
    static HBRUSH s_brCard;
    static HBRUSH s_brBorder;
};
