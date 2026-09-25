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

    // Returns one pseudo-entry per active display monitor ("Tum Ekran" capture targets).
    // hwnd stays null; the 'monitor' field identifies which HMONITOR to capture.
    static std::vector<WindowInfo> GetMonitors();

    // Hedef pencerenin sahibi process'in TAM exe yolunu dondurur (hata/pencere yoksa "").
    // App::ResolveTargetExeName'in aksine PathFindFileNameW ile KIRPILMAZ -- preset
    // eslestirmesi tam yol uzerinden yapildigi icin ayri tutuluyor.
    static std::wstring ResolveExeFullPath(HWND hwnd);

    // Bir HMONITOR'un kalici cihaz adini dondurur (orn. L"\\.\DISPLAY1"; hata/monitor
    // yoksa ""). Preset sisteminde "Tum Ekran" hedeflerinin kimligi olarak kullanilir.
    static std::wstring ResolveMonitorDeviceName(HMONITOR hMonitor);

    // 'target' ile ayni ekran alanini (targetRect) kaplayan, target'in ONUNDE
    // (z-order'da ustunde) baska bir gorunur pencere var mi? DXGI Desktop Duplication
    // + crop-to-window yakalamasinda kullanilir: WGC'nin CreateForWindow'unun aksine
    // duplication tum monitoru yakaladigindan, target'in ustune gelen (Discord bildirimi,
    // baska pencere vb.) HERHANGI bir seyi de yakalar. target'in KENDI process'ine ait
    // pencereler (oyunun kendi ic-overlay'i gibi) ve bizim kendi pencerelerimiz (overlay/
    // menu/settings) "yabanci isgal" sayilmaz, atlanir. Sadece yerel Win32 cagrilari, IPC yok.
    static bool IsOccluded(HWND target, const RECT& targetRect);

private:
    struct EnumData
    {
        std::vector<WindowInfo>* windows;
        HWND                     excludeHwnd;
    };

    static BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam);
};
