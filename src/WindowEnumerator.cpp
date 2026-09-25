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

static BOOL CALLBACK EnumMonitorsProc(HMONITOR hMon, HDC, LPRECT, LPARAM lParam)
{
    auto* monitors = reinterpret_cast<std::vector<WindowInfo>*>(lParam);

    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hMon, &mi)) return TRUE;

    int w = mi.rcMonitor.right  - mi.rcMonitor.left;
    int h = mi.rcMonitor.bottom - mi.rcMonitor.top;

    wchar_t title[128];
    swprintf_s(title, L"\U0001F5A5 Tüm Ekran%s (%dx%d)",
        (mi.dwFlags & MONITORINFOF_PRIMARY) ? L" - Birincil" : L"", w, h);

    WindowInfo info;
    info.hwnd    = nullptr;
    info.title   = title;
    info.icon    = nullptr;
    info.monitor = hMon;
    monitors->push_back(info);
    return TRUE;
}

std::vector<WindowInfo> WindowEnumerator::GetMonitors()
{
    std::vector<WindowInfo> monitors;
    EnumDisplayMonitors(nullptr, nullptr, EnumMonitorsProc, reinterpret_cast<LPARAM>(&monitors));
    return monitors;
}

std::wstring WindowEnumerator::ResolveExeFullPath(HWND hwnd)
{
    if (!hwnd) return L"";

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0) return L"";

    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProcess) return L"";

    wchar_t exePath[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    const BOOL ok = QueryFullProcessImageNameW(hProcess, 0, exePath, &size);
    CloseHandle(hProcess);

    return ok ? std::wstring(exePath) : std::wstring();
}

std::wstring WindowEnumerator::ResolveMonitorDeviceName(HMONITOR hMonitor)
{
    if (!hMonitor) return L"";

    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hMonitor, &mi)) return L"";

    return mi.szDevice;
}

bool WindowEnumerator::IsOccluded(HWND target, const RECT& targetRect)
{
    if (!target) return false;

    DWORD targetPid = 0;
    GetWindowThreadProcessId(target, &targetPid);
    const DWORD ourPid = GetCurrentProcessId();

    for (HWND hwnd = GetTopWindow(nullptr); hwnd; hwnd = GetWindow(hwnd, GW_HWNDNEXT))
    {
        if (hwnd == target) break; // reached the target's own z-order slot -- nothing above it left to check

        if (!IsWindowVisible(hwnd)) continue;
        if (IsIconic(hwnd)) continue;

        // Cloaked windows (other virtual desktop, suspended UWP, DWM thumbnail source) are
        // not actually visible on screen even though IsWindowVisible can say otherwise.
        BOOL cloaked = FALSE;
        if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked)
            continue;

        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        // Target's own windows (e.g. the game's own in-game overlay/child dialogs) and our
        // own windows (overlay/menu/settings/presets/hotkeys) are not "foreign occlusion".
        if (pid == targetPid || pid == ourPid) continue;

        // DWM "Ghost" windows: when the target briefly stops pumping messages (very common
        // right at startup/alt-tab/loading screens), Windows draws a last-good-frame ghost of
        // it OWNED BY EXPLORER.EXE at the exact same screen rect, on top, until it responds
        // again. That ghost has a DIFFERENT pid than both target and us, so without this check
        // it was misread as a real foreign occluder -- freezing the whole DXGI capture (skipping
        // every frame) for as long as the ghost lingers, which is exactly the multi-second
        // "capture frozen at startup" symptom seen in the field.
        wchar_t cls[64] = {};
        const bool haveClass = GetClassNameW(hwnd, cls, _countof(cls)) != 0;
        if (haveClass && wcscmp(cls, L"Ghost") == 0)
            continue;

        // Surucu/yardimci-uygulama overlay pencereleri: NVIDIA GeForce Experience'in
        // Alt+Z/ShadowPlay altyapisi icin surekli acik tuttugu "CEF-OSC-WIDGET" siniftaki
        // ("NVIDIA GeForce Overlay DT" basligiyla) pencere, GENELLIKLE tamamen seffaf/gorunmez
        // olsa da hedef pencereyle AYNI ekran dikdortgenini kaplayip kalici sekilde orada
        // duruyor -- kendi surecimize ait olmadigindan (ayri bir NVIDIA container process'i)
        // "yabanci isgal" sanilip DXGI capture'i SONSUZA KADAR durduruyordu (Ghost pencerenin
        // aksine bu asla kendiliginden kaybolmuyor). Alan basiliyken (gercek Alt+Z UI'si acik)
        // bir iki kare atlanmasi kabul edilebilir bir bedel; surekli orada durup capture'i
        // tamamen kilitlemesi degil.
        if (haveClass && wcscmp(cls, L"CEF-OSC-WIDGET") == 0)
            continue;

        RECT r = {};
        if (!GetWindowRect(hwnd, &r)) continue;

        RECT intersection;
        if (IntersectRect(&intersection, &r, &targetRect))
        {
            static bool s_occluderLogged = false;
            if (!s_occluderLogged)
            {
                s_occluderLogged = true;
                wchar_t title[128] = {};
                GetWindowTextW(hwnd, title, _countof(title));
                DLSS_Log("[Capture] Occluder tespit edildi: hwnd=0x%p sinif='%ls' baslik='%ls' pid=%lu "
                         "(hedef_pid=%lu, biz_pid=%lu)", hwnd, cls, title, pid, targetPid, ourPid);
            }
            return true;
        }
    }

    return false;
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

