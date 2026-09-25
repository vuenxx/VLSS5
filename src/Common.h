#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <dxgi1_5.h>
#include <dwmapi.h>
#include <wrl/client.h>
#include <string>
#include <vector>
#include <memory>
#include <atomic>
#include <stdexcept>
#include <cassert>
#include <timeapi.h>
#include <avrt.h>

// WinRT / Windows Graphics Capture
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <Windows.Graphics.Capture.Interop.h>
#include <Windows.Graphics.DirectX.Direct3D11.Interop.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "avrt.lib")

using Microsoft::WRL::ComPtr;

// -----------------------------------------------------------------------
// Shared data types
// -----------------------------------------------------------------------

struct WindowInfo
{
    HWND         hwnd = nullptr;
    std::wstring title;
    HICON        icon = nullptr;

    // Non-null only for a "whole screen" pseudo-entry (see WindowEnumerator::GetMonitors).
    // When set, 'hwnd' stays null and this monitor is captured instead of a specific window.
    HMONITOR     monitor = nullptr;
};

// Thin HRESULT guard — throws on failure (debug-friendly).
inline void ThrowIfFailed(HRESULT hr, const char* msg = "HRESULT failed")
{
    if (FAILED(hr))
        throw std::runtime_error(msg);
}

// Global logger for DLSS and DirectX diagnostics
void DLSS_Log(const char* fmt, ...);
