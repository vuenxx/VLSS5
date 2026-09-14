#pragma once
#include "Common.h"
#include "ConfigManager.h"
#include <functional>

class HotkeysWindow
{
public:
    static void Initialize(HINSTANCE hInstance);
    static void Show(HWND parent = nullptr);
    static void Hide();
    static bool IsOpen();
    static HWND GetHwnd() { return s_hwnd; }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK RebindKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);
    static void CreateControls(HWND hwnd);
    static void UpdateLabels();
    static void StartKeybindCapture(int hotkeyId);
    static void EndKeybindCapture(bool save, UINT vk, UINT mod);

    static HWND s_hwnd;
    static HINSTANCE s_hInstance;

    static bool s_rebindingKey;
    static HHOOK s_rebindHook;
    static int s_currentRebindId; // 0=Settings, 1=FG, 2=Focus, 3=FPS, 4=VLSS, 5=Calib, 6=Start

    static HWND s_lblSettings, s_btnSettings;
    static HWND s_lblFg, s_btnFg;
    static HWND s_lblFocus, s_btnFocus;
    static HWND s_lblFps, s_btnFps;
    static HWND s_lblVlss, s_btnVlss;
    static HWND s_lblCalib, s_btnCalib;
    static HWND s_lblStart, s_btnStart;

    static HWND s_btnClose;

    static HFONT s_fontTitle;
    static HFONT s_fontNormal;
    static HFONT s_fontBold;
};
