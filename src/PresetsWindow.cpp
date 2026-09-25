#include "PresetsWindow.h"
#include "ConfirmDialog.h"
#include "../third_party/json/json.hpp"
#include <shellapi.h>
#include <algorithm>

#pragma comment(lib, "shell32.lib")

using json = nlohmann::json;

std::unique_ptr<WebViewHost> PresetsWindow::s_host;
HINSTANCE PresetsWindow::s_hInstance = nullptr;
std::vector<PresetEntry> PresetsWindow::s_cache;

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

void PresetsWindow::Initialize(HINSTANCE hInstance)
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

void PresetsWindow::Show(HWND parent)
{
    if (!s_host)
    {
        s_host = std::make_unique<WebViewHost>();
        s_host->CreatePopup(parent, L"Ön Ayarlar",
            L"vlss5.presets", ExeDirWebFolder(L"presets").c_str(), L"index.html",
            460, 400, &PresetsWindow::OnWebMessage);
    }
    else
    {
        s_host->Show();
    }
    PushListToJs();
}

void PresetsWindow::Hide()
{
    if (s_host) s_host->Hide();
}

bool PresetsWindow::IsOpen()
{
    return s_host && s_host->IsOpen();
}

HWND PresetsWindow::GetHwnd()
{
    return s_host ? s_host->GetHwnd() : nullptr;
}

void PresetsWindow::PushListToJs()
{
    if (!s_host) return;

    s_cache = PresetsManager::Get().ListPresets();

    json arr = json::array();
    for (auto& e : s_cache)
    {
        json item;
        item["folder"]      = ToUtf8(e.folderName);
        item["displayName"] = ToUtf8(e.displayName);
        arr.push_back(item);
    }

    json msg;
    msg["type"] = "presetList";
    msg["data"] = arr;
    s_host->PostJson(FromUtf8(msg.dump()));
}

void PresetsWindow::OnWebMessage(const std::wstring& jsonStr)
{
    json msg;
    try { msg = json::parse(ToUtf8(jsonStr)); }
    catch (...) { return; }

    try { DispatchMessage(msg); }
    catch (...) { /* Hatali/beklenmedik JSON alani -- bu mesaji yoksay, uygulamayi cokertme. */ }
}

void PresetsWindow::DispatchMessage(const json& msg)
{
    std::string cmd = msg.value("cmd", "");
    HWND hwnd = s_host ? s_host->GetHwnd() : nullptr;

    if (cmd == "close")
    {
        Hide();
    }
    else if (cmd == "listPresets")
    {
        PushListToJs();
    }
    else if (cmd == "openFolder")
    {
        std::wstring dir = PresetsManager::Get().GetPresetsRootDir();
        ShellExecuteW(hwnd, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    else if (cmd == "deletePreset")
    {
        std::wstring folder = FromUtf8(msg.value("folder", std::string()));
        if (folder.empty()) return;

        auto it = std::find_if(s_cache.begin(), s_cache.end(),
            [&](const PresetEntry& e) { return e.folderName == folder; });
        std::wstring displayName = (it != s_cache.end()) ? it->displayName : folder;

        std::wstring dispMsg = L"\"" + displayName + L"\" silinsin mi?";
        if (ConfirmDialog::AskYesNo(hwnd, L"VLSS5 - Ön Ayarı Sil",
                L"Bu ön ayarı silmek istediğinizden emin misiniz?", dispMsg.c_str(),
                L"Evet", L"Hayır", /*defaultIsYes*/false, /*warningIcon*/true))
        {
            PresetsManager::Get().DeletePreset(folder);
            PushListToJs();
        }
    }
}
