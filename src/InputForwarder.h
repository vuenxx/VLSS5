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
//  The keybind-assignment flow (BeginCapture / EndCapture) used to install a
//  temporary WH_KEYBOARD_LL hook while the menu UI waited for a key. That is
//  GONE too now -- a system-wide keyboard hook, even a short-lived one, is
//  one of the single strongest behavioral signals AV heuristics (incl.
//  Defender's Wacatac.B!ml) associate with keyloggers, especially from an
//  elevated/admin process. Rebinding is now done entirely via the WebView2
//  UI's own "keydown" DOM event (bkz. HotkeysWindow.cpp, web/hotkeys/app.js,
//  web/main/app.js) -- zero Win32 hook APIs anywhere in this codebase.
//  BeginCapture/EndCapture below just track "are we currently waiting for a
//  key" state for that JS-driven flow; they install nothing.
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
    static HotkeyConfig                       s_config;
    static bool                               s_capturing;
    static std::function<void(HotkeyConfig)>  s_captureCallback;
};
