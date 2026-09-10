#include "InputForwarder.h"

// -----------------------------------------------------------------------
// Static member definitions
// -----------------------------------------------------------------------
HotkeyConfig                       InputForwarder::s_config{};
bool                               InputForwarder::s_capturing     = false;
HHOOK                              InputForwarder::s_captureHook   = nullptr;
std::function<void(HotkeyConfig)>  InputForwarder::s_captureCallback;

// -----------------------------------------------------------------------
// HotkeyConfig::FormatDisplay
// -----------------------------------------------------------------------
std::wstring HotkeyConfig::FormatDisplay() const
{
    std::wstring result;

    if (modifiers & MOD_CONTROL) result += L"Ctrl+";
    if (modifiers & MOD_ALT)     result += L"Alt+";
    if (modifiers & MOD_SHIFT)   result += L"Shift+";
    if (modifiers & MOD_WIN)     result += L"Win+";

    UINT   scanCode = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    LPARAM lp       = static_cast<LPARAM>(scanCode << 16);

    // Extended-key flag for keys that need it
    if (vk >= VK_F1    && vk <= VK_F24)  lp |= (1LL << 24);
    if (vk == VK_INSERT || vk == VK_DELETE ||
        vk == VK_HOME   || vk == VK_END    ||
        vk == VK_PRIOR  || vk == VK_NEXT   ||
        vk == VK_LEFT   || vk == VK_RIGHT  ||
        vk == VK_UP     || vk == VK_DOWN)   lp |= (1LL << 24);

    wchar_t keyName[128] = {};
    if (GetKeyNameTextW(static_cast<LONG>(lp), keyName, _countof(keyName)) > 0)
        result += keyName;
    else if (vk >= 0x20 && vk < 0x7F)
        result += static_cast<wchar_t>(vk);
    else
    {
        wchar_t hex[16];
        swprintf_s(hex, L"VK[%02X]", vk);
        result += hex;
    }

    return result;
}

// -----------------------------------------------------------------------
// IsStopKeyDown
//   Polls the physical key state directly — no hooks, no message queue
//   involvement, no interference with ANY other application.
//
//   GetAsyncKeyState reads the hardware key state as of the call.
//   The game / ReShade / DirectInput see the exact same key state.
// -----------------------------------------------------------------------
bool InputForwarder::IsStopKeyDown()
{
    // Main key must be physically held
    if (!(GetAsyncKeyState(static_cast<int>(s_config.vk)) & 0x8000))
        return false;

    // Strip MOD_NOREPEAT — that flag has no meaning for GetAsyncKeyState
    const UINT mods = s_config.modifiers & ~static_cast<UINT>(MOD_NOREPEAT);

    if ((mods & MOD_CONTROL) && !(GetAsyncKeyState(VK_CONTROL) & 0x8000)) return false;
    if ((mods & MOD_ALT)     && !(GetAsyncKeyState(VK_MENU)    & 0x8000)) return false;
    if ((mods & MOD_SHIFT)   && !(GetAsyncKeyState(VK_SHIFT)   & 0x8000)) return false;
    if ((mods & MOD_WIN) &&
        !((GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000))
        return false;

    return true;
}

// -----------------------------------------------------------------------
// BeginCapture / EndCapture
//   Used ONLY in the menu UI while assigning a new keybind.
//   The hook is removed the moment a key is captured.
// -----------------------------------------------------------------------
void InputForwarder::BeginCapture(std::function<void(HotkeyConfig)> callback)
{
    if (s_capturing) EndCapture();
    s_captureCallback = std::move(callback);
    s_capturing       = true;
    s_captureHook     = SetWindowsHookExW(
        WH_KEYBOARD_LL, CaptureKeyboardProc,
        GetModuleHandleW(nullptr), 0);
}

void InputForwarder::EndCapture()
{
    if (s_captureHook)
    {
        UnhookWindowsHookEx(s_captureHook);
        s_captureHook = nullptr;
    }
    s_captureCallback = nullptr;
    s_capturing       = false;
}

LRESULT CALLBACK InputForwarder::CaptureKeyboardProc(
    int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION &&
        (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN))
    {
        auto* kb = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
        UINT  vk = kb->vkCode;

        // Ignore bare modifier keys
        const bool isMod =
            (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL ||
             vk == VK_MENU    || vk == VK_LMENU    || vk == VK_RMENU    ||
             vk == VK_SHIFT   || vk == VK_LSHIFT   || vk == VK_RSHIFT   ||
             vk == VK_LWIN    || vk == VK_RWIN);

        if (!isMod)
        {
            UINT mods = MOD_NOREPEAT;
            if (GetAsyncKeyState(VK_CONTROL) & 0x8000) mods |= MOD_CONTROL;
            if (GetAsyncKeyState(VK_MENU)    & 0x8000) mods |= MOD_ALT;
            if (GetAsyncKeyState(VK_SHIFT)   & 0x8000) mods |= MOD_SHIFT;
            if ((GetAsyncKeyState(VK_LWIN) |
                 GetAsyncKeyState(VK_RWIN)) & 0x8000)  mods |= MOD_WIN;

            HotkeyConfig newCfg = { mods, vk };
            s_config = newCfg;

            auto cb = std::move(s_captureCallback);
            EndCapture();   // unhook BEFORE firing callback
            if (cb) cb(newCfg);

            return 1;       // consume during capture only
        }
    }
    return CallNextHookEx(s_captureHook, nCode, wParam, lParam);
}
