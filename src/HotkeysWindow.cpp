#include "HotkeysWindow.h"
#include "../third_party/json/json.hpp"

using json = nlohmann::json;

std::unique_ptr<WebViewHost> HotkeysWindow::s_host;
HINSTANCE HotkeysWindow::s_hInstance = nullptr;
bool HotkeysWindow::s_rebindingKey = false;
HHOOK HotkeysWindow::s_rebindHook = nullptr;
int HotkeysWindow::s_currentRebindId = -1;
HotkeysWindow::KeyCapturedCallback HotkeysWindow::s_externalCallback = nullptr;

void HotkeysWindow::SetExternalKeyCapturedCallback(KeyCapturedCallback cb)
{
    s_externalCallback = std::move(cb);
}

static std::string ToUtf8(const std::wstring& w)
{
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(len > 0 ? len - 1 : 0, '\0');
    if (len > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), len, nullptr, nullptr);
    return out;
}
static std::wstring FromUtf8(const std::string& s)
{
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring out(len > 0 ? len - 1 : 0, L'\0');
    if (len > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), len);
    return out;
}

void HotkeysWindow::Initialize(HINSTANCE hInstance)
{
    s_hInstance = hInstance;
    WebViewHost::EnsureEnvironment(hInstance);
}

static std::wstring ExeDirWebFolder(const wchar_t* subfolder)
{
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir = path;
    size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) dir = dir.substr(0, slash);
    return dir + L"\\web\\" + subfolder;
}

void HotkeysWindow::Show(HWND parent)
{
    if (!s_host)
    {
        s_host = std::make_unique<WebViewHost>();
        s_host->CreatePopup(parent, L"Tuşları Değiştir",
            L"vlss5.hotkeys", ExeDirWebFolder(L"hotkeys").c_str(), L"index.html",
            420, 420, &HotkeysWindow::OnWebMessage);
    }
    else
    {
        s_host->Show();
    }
    PushHotkeysToJs();
}

void HotkeysWindow::Hide()
{
    if (s_rebindingKey) EndKeybindCapture(false, 0, 0);
    if (s_host) s_host->Hide();
}

bool HotkeysWindow::IsOpen()
{
    return s_host && s_host->IsOpen();
}

HWND HotkeysWindow::GetHwnd()
{
    return s_host ? s_host->GetHwnd() : nullptr;
}

void HotkeysWindow::PushHotkeysToJs()
{
    if (!s_host) return;
    auto& cfg = ConfigManager::Get().Config();

    json labels;
    labels["0"] = ToUtf8(Dlss5Config::FormatKey(cfg.settingsVk, cfg.settingsMod));
    labels["2"] = ToUtf8(Dlss5Config::FormatKey(cfg.vkFocus));
    labels["3"] = ToUtf8(Dlss5Config::FormatKey(cfg.vkFps));
    labels["4"] = ToUtf8(Dlss5Config::FormatKey(cfg.vkToggleVlss));
    labels["5"] = ToUtf8(Dlss5Config::FormatKey(cfg.vkCalib));
    labels["6"] = ToUtf8(Dlss5Config::FormatKey(cfg.vkStart, cfg.modStart));
    labels["7"] = ToUtf8(Dlss5Config::FormatKey(cfg.vkDismissWarning));

    json msg;
    msg["type"] = "hotkeys";
    msg["data"] = labels;
    s_host->PostJson(FromUtf8(msg.dump()));
}

void HotkeysWindow::OnWebMessage(const std::wstring& jsonStr)
{
    json msg;
    try { msg = json::parse(ToUtf8(jsonStr)); }
    catch (...) { return; }

    try
    {
        std::string cmd = msg.value("cmd", "");

        if (cmd == "close")
        {
            Hide();
        }
        else if (cmd == "getHotkeys")
        {
            PushHotkeysToJs();
        }
        else if (cmd == "startCapture")
        {
            int id = msg.value("hotkeyId", -1);
            StartKeybindCapture(id);
        }
    }
    catch (...) { /* Hatali/beklenmedik JSON alani -- bu mesaji yoksay, uygulamayi cokertme. */ }
}

LRESULT CALLBACK HotkeysWindow::RebindKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION && s_rebindingKey && (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN))
    {
        auto* kb = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
        UINT vk = kb->vkCode;

        if (vk == VK_ESCAPE)
        {
            EndKeybindCapture(false, 0, 0);
            return 1;
        }
        else if (vk != VK_LSHIFT && vk != VK_RSHIFT && vk != VK_SHIFT &&
                 vk != VK_LCONTROL && vk != VK_RCONTROL && vk != VK_CONTROL &&
                 vk != VK_LMENU && vk != VK_RMENU && vk != VK_MENU)
        {
            UINT mods = 0;
            if (GetAsyncKeyState(VK_CONTROL) & 0x8000) mods |= MOD_CONTROL;
            if (GetAsyncKeyState(VK_MENU)    & 0x8000) mods |= MOD_ALT;
            if (GetAsyncKeyState(VK_SHIFT)   & 0x8000) mods |= MOD_SHIFT;

            EndKeybindCapture(true, vk, mods);
            return 1;
        }
    }
    return CallNextHookEx(s_rebindHook, nCode, wParam, lParam);
}

void HotkeysWindow::StartKeybindCapture(int hotkeyId)
{
    if (s_rebindingKey) return;
    s_currentRebindId = hotkeyId;
    s_rebindingKey = true;
    s_rebindHook = SetWindowsHookExW(WH_KEYBOARD_LL, RebindKeyboardProc, GetModuleHandle(nullptr), 0);
}

void HotkeysWindow::EndKeybindCapture(bool save, UINT vk, UINT mod)
{
    if (!s_rebindingKey) return;
    s_rebindingKey = false;
    if (s_rebindHook)
    {
        UnhookWindowsHookEx(s_rebindHook);
        s_rebindHook = nullptr;
    }

    if (save)
    {
        auto& cfg = ConfigManager::Get().Config();
        if (s_currentRebindId == 0) { cfg.settingsVk = vk; cfg.settingsMod = mod; }
        if (s_currentRebindId == 2) { cfg.vkFocus = vk; }
        if (s_currentRebindId == 3) { cfg.vkFps = vk; }
        if (s_currentRebindId == 4) { cfg.vkToggleVlss = vk; }
        if (s_currentRebindId == 5) { cfg.vkCalib = vk; }
        if (s_currentRebindId == 6) { cfg.vkStart = vk; cfg.modStart = mod; }
        if (s_currentRebindId == 7) { cfg.vkDismissWarning = vk; }

        ConfigManager::Get().Save();

        // Let Main window know we updated start key
        HWND hMain = FindWindowW(L"VLSS5Main", nullptr);
        if (hMain) PostMessageW(hMain, WM_APP + 1, 0, 0);
    }

    if (s_host)
    {
        json ack;
        ack["type"] = "keyCaptured";
        s_host->PostJson(FromUtf8(ack.dump()));
    }
    PushHotkeysToJs();

    // Faz 3: ana pencere sekmesi (varsa) FARKLI bir WebViewHost kullaniyor --
    // s_host'un yukarida yaptigi PostJson cagrilarindan hicbiri ona ulasmaz.
    // Main.cpp bu callback'i bir kez register eder (WinMain, Initialize sonrasi).
    if (s_externalCallback)
        s_externalCallback(save, vk, mod);
}
