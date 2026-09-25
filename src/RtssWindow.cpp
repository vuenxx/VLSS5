#include "RtssWindow.h"
#include "ConfirmDialog.h"
#include "../third_party/json/json.hpp"
#include <shobjidl.h>
#include <shlwapi.h>

#pragma comment(lib, "shlwapi.lib")

using json = nlohmann::json;

std::unique_ptr<WebViewHost> RtssWindow::s_host;
HINSTANCE RtssWindow::s_hInstance = nullptr;

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

void RtssWindow::Initialize(HINSTANCE hInstance)
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

void RtssWindow::Show(HWND parent)
{
    if (!s_host)
    {
        s_host = std::make_unique<WebViewHost>();
        s_host->CreatePopup(parent, L"RTSS Ayarları",
            L"vlss5.rtss", ExeDirWebFolder(L"rtss").c_str(), L"index.html",
            460, 340, &RtssWindow::OnWebMessage);
    }
    else
    {
        s_host->Show();
    }
    PushDirToJs();
    PushProfilesToJs();
}

void RtssWindow::Hide()
{
    if (s_host) s_host->Hide();
}

bool RtssWindow::IsOpen()
{
    return s_host && s_host->IsOpen();
}

HWND RtssWindow::GetHwnd()
{
    return s_host ? s_host->GetHwnd() : nullptr;
}

void RtssWindow::PushDirToJs()
{
    if (!s_host) return;

    std::wstring dir = ConfigManager::Get().Config().rtssDirectory;
    std::wstring shown = dir.empty() ? (L"(Varsayılan) " + RTSSManager::Get().GetProfilesDir()) : dir;

    wchar_t buf[440] = {};
    wcsncpy_s(buf, shown.c_str(), _TRUNCATE);
    PathCompactPathExW(buf, shown.c_str(), 70, 0);

    json msg;
    msg["type"] = "rtssDir";
    msg["data"] = ToUtf8(buf);
    s_host->PostJson(FromUtf8(msg.dump()));
}

void RtssWindow::PushProfilesToJs()
{
    if (!s_host) return;

    json arr = json::array();
    for (const auto& prof : RTSSManager::Get().GetProfiles())
        arr.push_back(ToUtf8(prof));

    json msg;
    msg["type"] = "profileList";
    msg["data"] = arr;
    s_host->PostJson(FromUtf8(msg.dump()));
}

void RtssWindow::OnWebMessage(const std::wstring& jsonStr)
{
    json msg;
    try { msg = json::parse(ToUtf8(jsonStr)); }
    catch (...) { return; }

    try { DispatchMessage(msg); }
    catch (...) { /* Hatali/beklenmedik JSON alani -- bu mesaji yoksay, uygulamayi cokertme. */ }
}

void RtssWindow::DispatchMessage(const json& msg)
{
    std::string cmd = msg.value("cmd", "");
    HWND hwnd = s_host ? s_host->GetHwnd() : nullptr;

    if (cmd == "close")
    {
        Hide();
    }
    else if (cmd == "getRtssDir")
    {
        PushDirToJs();
    }
    else if (cmd == "listProfiles")
    {
        PushProfilesToJs();
    }
    else if (cmd == "browseFolder")
    {
        IFileDialog* pfd = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pfd))))
        {
            DWORD dwOptions;
            if (SUCCEEDED(pfd->GetOptions(&dwOptions))) pfd->SetOptions(dwOptions | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
            if (SUCCEEDED(pfd->Show(hwnd)))
            {
                IShellItem* psi = nullptr;
                if (SUCCEEDED(pfd->GetResult(&psi)))
                {
                    PWSTR pszPath = nullptr;
                    if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &pszPath)))
                    {
                        ConfigManager::Get().Config().rtssDirectory = pszPath;
                        ConfigManager::Get().Save();
                        CoTaskMemFree(pszPath);
                        PushProfilesToJs();
                        PushDirToJs();
                    }
                    psi->Release();
                }
            }
            pfd->Release();
        }
    }
    else if (cmd == "deleteProfile")
    {
        std::wstring name = FromUtf8(msg.value("name", std::string()));
        if (name.empty()) return;

        std::wstring dispMsg = L"\"" + name + L"\" profili silinsin mi?";
        if (ConfirmDialog::AskYesNo(hwnd, L"VLSS5 - Profili Sil",
                L"Bu RTSS profilini silmek istediğinizden emin misiniz?", dispMsg.c_str(),
                L"Evet", L"Hayır", /*defaultIsYes*/false, /*warningIcon*/true))
        {
            std::wstring cfgPath = RTSSManager::Get().GetProfilesDir() + L"\\" + name + L".cfg";
            DeleteFileW(cfgPath.c_str());
            RTSSManager::Get().NotifyRTSS();
            PushProfilesToJs();
        }
    }
}
