#pragma once
#include "Common.h"
#include <functional>

// ---------------------------------------------------------------------------
// HotkeyConfig
// ---------------------------------------------------------------------------
struct HotkeyConfig
{
    UINT modifiers = MOD_ALT | MOD_NOREPEAT;
    UINT vk        = 'S';

    std::wstring FormatDisplay() const;

    // True when at least one real modifier (Ctrl/Alt/Shift/Win) is configured.
    bool HasRealModifier() const
    {
        return (modifiers & (MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN)) != 0;
    }
};

// ---------------------------------------------------------------------------
// InputForwarder
//
//  *** NO WH_KEYBOARD_LL hook during overlay operation. ***
//
//  The stop keybind is detected via GetAsyncKeyState() polling inside the
//  render loop (App::Update). This approach:
//    - Installs ZERO hooks while the overlay is running
//    - Does NOT intercept, consume, or reorder any keypress
//    - ReShade, games, and every other app receive ALL keys untouched
//    - Works for bare keys (Insert) and modifier combos (Alt+S)
//
//  A WH_KEYBOARD_LL hook is installed ONLY during the brief keybind-
//  assignment flow in the menu UI (BeginCapture / EndCapture), and it is
//  removed the moment the user presses a key.
// ---------------------------------------------------------------------------
class InputForwarder
{
public:
    static HotkeyConfig& Config() { return s_config; }

    // Poll whether the configured stop combination is currently held down.
    // Call once per frame from the render loop.  No hooks, no side effects.
    static bool IsStopKeyDown();

    // ---- Capture flow (menu UI only) ----
    static void BeginCapture(std::function<void(HotkeyConfig)> callback);
    static void EndCapture();
    static bool IsCapturing() { return s_capturing; }

private:
    static LRESULT CALLBACK CaptureKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);

    static HotkeyConfig                       s_config;
    static bool                               s_capturing;
    static HHOOK                              s_captureHook;
    static std::function<void(HotkeyConfig)>  s_captureCallback;
};
