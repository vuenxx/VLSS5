#include "WindowEnumerator.h"
#include <shellapi.h>
#pragma comment(lib, "shell32.lib")

static HICON FetchWindowIcon(HWND hwnd)
{
    HICON hIcon = nullptr;

    // 1. SendMessageTimeout for WM_GETICON (fast timeout prevents hanging on frozen windows)
    DWORD_PTR res = 0;
    if (SendMessageTimeoutW(hwnd, WM_GETICON, ICON_SMALL2, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 100, &res) && res)
        hIcon = reinterpret_cast<HICON>(res);

    if (!hIcon && SendMessageTimeoutW(hwnd, WM_GETICON, ICON_SMALL, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 100, &res) && res)
        hIcon = reinterpret_cast<HICON>(res);

    if (!hIcon && SendMessageTimeoutW(hwnd, WM_GETICON, ICON_BIG, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 100, &res) && res)
        hIcon = reinterpret_cast<HICON>(res);

    // 2. Class icons
    if (!hIcon)
        hIcon = reinterpret_cast<HICON>(GetClassLongPtrW(hwnd, GCLP_HICONSM));
    if (!hIcon)
        hIcon = reinterpret_cast<HICON>(GetClassLongPtrW(hwnd, GCLP_HICON));

    // 3. Process executable icon
    if (!hIcon)
    {
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid != 0)
        {
            HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (hProc)
            {
                wchar_t exePath[MAX_PATH] = {};
                DWORD size = MAX_PATH;
                if (QueryFullProcessImageNameW(hProc, 0, exePath, &size))
                {
                    WORD iconIdx = 0;
                    hIcon = ExtractAssociatedIconW(GetModuleHandleW(nullptr), exePath, &iconIdx);
                }
                CloseHandle(hProc);
            }
        }
    }

    // 4. Fallback application icon
    if (!hIcon)
    {
        hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    }

    return hIcon;
}

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

    HICON icon = FetchWindowIcon(hwnd);

    data->windows->push_back({ hwnd, title, icon });
    return TRUE;
}

