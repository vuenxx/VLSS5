#pragma once
#include "Common.h"

// ---------------------------------------------------------------------------
// WindowEnumerator
//   Enumerates all visible, non-tool, top-level application windows.
// ---------------------------------------------------------------------------
class WindowEnumerator
{
public:
    // Returns a list of visible top-level windows.
    // Pass a HWND to exclude (e.g. our own menu window).
    static std::vector<WindowInfo> GetWindows(HWND excludeHwnd = nullptr);

private:
    struct EnumData
    {
        std::vector<WindowInfo>* windows;
        HWND                     excludeHwnd;
    };

    static BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam);
};
