#include "WindowEnumerator.h"

std::vector<WindowInfo> WindowEnumerator::GetWindows(HWND excludeHwnd)
{
    std::vector<WindowInfo> windows;
    EnumData data = { &windows, excludeHwnd };
    EnumWindows(EnumWindowsProc, reinterpret_cast<LPARAM>(&data));
    return windows;
}

BOOL CALLBACK WindowEnumerator::EnumWindowsProc(HWND hwnd, LPARAM lParam)
{
    auto* data = reinterpret_cast<EnumData*>(lParam);

    // Skip our own window
    if (hwnd == data->excludeHwnd) return TRUE;

    // Must be visible and not minimized
    if (!IsWindowVisible(hwnd))  return TRUE;
    if (IsIconic(hwnd))          return TRUE;

    // Must have a title
    if (GetWindowTextLengthW(hwnd) == 0) return TRUE;

    // Skip owned windows (dialogs, popups that belong to another top-level)
    if (GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;

    // Skip tool windows (system tray, etc.)
    DWORD exStyle = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
    if (exStyle & WS_EX_TOOLWINDOW) return TRUE;

    wchar_t title[512] = {};
    GetWindowTextW(hwnd, title, _countof(title));

    data->windows->push_back({ hwnd, title });
    return TRUE;
}
