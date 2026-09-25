#include "Common.h"
#include "App.h"
#include "WindowEnumerator.h"
#include "DLSSManager.h"
#include "ConfigManager.h"
#include "SettingsWindow.h"
#include "RtssWindow.h"
#include "HotkeysWindow.h"
#include "PresetsWindow.h"
#include "RTSSManager.h"
#include "CrashHandler.h"
#include "WebViewHost.h"
#include "ConfirmDialog.h"
#include "PresetsManager.h"
#include "UpdateChecker.h"
#include "Version.h"
#include "resource.h"
#include "../third_party/json/json.hpp"
#include <shlwapi.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <objidl.h>
#include <gdiplus.h>
#include <thread>
#pragma comment(lib, "gdiplus.lib")

using json = nlohmann::json;

// Common Controls v6 (side-by-side) manifest baglantisi. Bu olmadan uygulama
// System32'deki eski v5 comctl32.dll'i yukler; bu surumde TaskDialogIndirect
// export edilmedigi icin GetProcAddress hep NULL doner ve tum TaskDialog
// cagrilari (dogrulama checkbox'i dahil) sessizce eski MessageBoxW'a duser.
#pragma comment(linker, \
    "\"/manifestdependency:type='win32' "                    \
    "name='Microsoft.Windows.Common-Controls' "               \
    "version='6.0.0.0' processorArchitecture='*' "             \
    "publicKeyToken='6595b64144ccf1df' language='*'\"")
#include <TlHelp32.h>
#include <algorithm>
#include <cwctype>
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")

// ---------------------------------------------------------------------------
// Control IDs
// ---------------------------------------------------------------------------
#define ID_GLOBAL_HOTKEY      201   // Global capture toggle hotkey
#define IDT_HOTKEY_TIMER      301   // Fallback hotkey poller (50ms)

// UpdateChecker arka plan thread'lerinin UI thread'ine geri bildirim mesajlari
// (bkz. WM_APP+4 icin yorum -- reentrant WebView2 COM cagrisi sorunuyla ayni
// gerekceyle: agir/asenkron isler her zaman mesaj dongusune dondukten sonra islenir).
#define WM_APP_UPDATES_FETCH_DONE      (WM_APP + 5) // lParam = UpdateChecker::FetchResult*, wParam = 1 ise sessiz (startup) kontrol
#define WM_APP_UPDATES_DL_PROGRESS     (WM_APP + 6) // lParam = UpdateChecker::DownloadProgress*
#define WM_APP_UPDATES_DL_DONE         (WM_APP + 7) // lParam = std::wstring* (basariliysa dosya yolu, degilse hata metni), wParam = basari (1/0)

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
static std::unique_ptr<App>    g_app;
static std::vector<WindowInfo> g_windows;
// g_windows ile PARALEL, ayni index'e sahip HICON->PNG data-URI onbellegi.
// Web listesi her hedefin simgesini burdan okur (PopulateList doldurur).
static std::vector<std::string> g_windowIcons;

struct GpuAdapterInfo
{
    std::wstring name;
    std::wstring displayName;
};
static std::vector<GpuAdapterInfo> g_gpuList;

static HWND                    g_mainHwnd        = nullptr;

// Ana pencerenin istemci alanini kaplayan gomulu WebView2 kontrolu (Faz 2).
static std::unique_ptr<WebViewHost> g_mainWebView;

// Eskiden ListBox_GetCurSel/CB_GETCURSEL ile okunan secimler artik JS
// tarafindan "selectTarget"/"setGpu"/"setDlssGpu" komutlariyla bildirilip
// burada native tarafta ayna tutuluyor (TriggerCaptureToggle/BASLAT akisi
// bunlara native taraftan erisebilsin diye).
static int                     g_selectedTargetIndex   = -1;
static int                     g_selectedGpuIndex      = 0;
static int                     g_selectedDlssGpuIndex  = 0;
static std::wstring            g_statusText = L"Hedef uygulamayı seçin veya istediğiniz penceredeyken ALT+S basın.";

static bool                    g_vsyncEnabled = false;
static bool                    g_fpsEnabled   = true;
static bool                    g_dlssEnabled  = true;
// Kalici: ConfigManager uzerinden yuklenir/kaydedilir.
static bool                    g_fullscreenStretch = false;

// Faz 3: ana pencere sekmelerinin ("Ön Ayarlar") en son native'e gonderdigi
// on ayar listesinin onbellegi -- PresetsWindow::s_cache ile AYNI amacla
// (silme onay dialogunda displayName gostermek icin), ama tamamen AYRI
// (SettingsWindow/PresetsWindow'un statik hallerine dokunulmuyor).
static std::vector<PresetEntry> g_presetsTabCache;

// ---------------------------------------------------------------------------
// "Güncellemeler" sekmesi -- bkz. UpdateChecker.h/.cpp. Ag cagrilari her zaman
// bir arka plan thread'inde yapilir, sonuclar WM_APP_UPDATES_* mesajlariyla
// UI thread'ine (WndProc) geri doner (bkz. WM_APP+4 reentrancy notu).
// ---------------------------------------------------------------------------
static bool                                    g_updatesChecking    = false;
static bool                                    g_updatesDownloading = false;
static bool                                    g_updatesHasNewer    = false;
static std::wstring                            g_updatesLatestTag;
static std::wstring                            g_updatesLastError;
static std::vector<UpdateChecker::ReleaseInfo> g_updatesCache;

// ---------------------------------------------------------------------------
// nvngx_dlssnr.dll surukle-birak yukleme durumu (bkz. dllStatusBar, Main.cpp
// "nvngxDropBegin/Chunk/End"). Tek seferde tek transfer varsayimi yeterli --
// JS tarafi zaten kendi "uploading" bayragiyla ikinci bir surumeyi engelliyor.
// ---------------------------------------------------------------------------
static HANDLE        g_nvngxUploadFile         = INVALID_HANDLE_VALUE;
static std::wstring  g_nvngxUploadTempPath;
static uint64_t      g_nvngxUploadExpectedSize = 0;
static uint64_t      g_nvngxUploadReceivedSize = 0;
static int           g_nvngxUploadNextSeq      = 0;

// ---------------------------------------------------------------------------
// UTF-8 helpers (JSON string alanlari icin) -- Faz 1'deki 4 pencerede oldugu
// gibi burada da AYRI kopyalanir, ortak bir header'a cikarilmaz.
// ---------------------------------------------------------------------------
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

// nvngx_dlssnr.dll (telifli NGX model agirliklari) -- kullanici tarafindan
// Ana Sayfa'daki durum/surukle-birak cubugundan eklenir (bkz. dllStatusBar,
// "nvngxDropBegin/Chunk/End"). Yoksa BASLAT hem JS'te hem burada engellenir.
static std::wstring GetNvngxDlssnrPath()
{
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    PathRemoveFileSpecW(exePath);
    wchar_t full[MAX_PATH] = {};
    PathCombineW(full, exePath, L"nvngx_dlssnr.dll");
    return full;
}

static bool NvngxDlssnrExists()
{
    return GetFileAttributesW(GetNvngxDlssnrPath().c_str()) != INVALID_FILE_ATTRIBUTES;
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

// IsOverlayRunning'in gercek tanimi asagida ("Hotkey & Capture Management"
// bolumunde), ContainsCaseInsensitive'in gercek tanimi asagida ("Helpers"
// bolumunde) -- BuildStateJson ikisinden once tanimlaniyor, bu yuzden ileri
// bildirim gerekiyor.
static bool IsOverlayRunning();
static bool ContainsCaseInsensitive(const std::wstring& str, const std::wstring& search);

// ---------------------------------------------------------------------------
// Native -> JS state push
// ---------------------------------------------------------------------------
static json BuildStateJson()
{
    json data;

    json targets = json::array();
    for (size_t i = 0; i < g_windows.size(); ++i)
    {
        json t;
        t["id"]        = static_cast<int>(i);
        t["label"]     = ToUtf8(g_windows[i].title);
        t["isMonitor"] = (g_windows[i].monitor != nullptr);
        t["icon"]      = (i < g_windowIcons.size()) ? g_windowIcons[i] : std::string();
        targets.push_back(t);
    }
    data["targets"]          = targets;
    data["selectedTargetId"] = g_selectedTargetIndex;

    json gpuList = json::array();
    for (auto& g : g_gpuList)
        gpuList.push_back(ToUtf8(g.displayName));
    data["gpuList"]     = gpuList;
    data["selectedGpu"] = g_selectedGpuIndex;

    json dlssGpuList = json::array();
    for (size_t idx = 0; idx < g_gpuList.size(); ++idx)
    {
        std::wstring label = g_gpuList[idx].displayName;
        if (idx > 0 && !ContainsCaseInsensitive(g_gpuList[idx].name, L"RTX"))
            label += L"  — DLSS yok";
        dlssGpuList.push_back(ToUtf8(label));
    }
    data["dlssGpuList"]     = dlssGpuList;
    data["selectedDlssGpu"] = g_selectedDlssGpuIndex;

    // Yakalama Yontemi: bkz. WM_CREATE'teki eski yorum -- WGC her zaman
    // varsayilan ve TEK secenek, combo kilitli/devre disi gosterilir.
    data["captureBackendList"]     = json::array({ ToUtf8(L"WGC (Varsayılan)") });
    data["selectedCaptureBackend"] = 0;
    data["captureBackendLocked"]   = true;

    data["vsync"]             = g_vsyncEnabled;
    data["fps"]               = g_fpsEnabled;
    data["dlss"]              = g_dlssEnabled;
    data["fullscreenStretch"] = g_fullscreenStretch;

    bool running = IsOverlayRunning();
    data["isCapturing"]        = running;
    data["settingsBtnEnabled"] = !running;
    data["nvngxDlssnrReady"]   = NvngxDlssnrExists();

    auto& c = ConfigManager::Get().Config();
    json hotkeyLabels;
    hotkeyLabels["focus"]      = ToUtf8(Dlss5Config::FormatKey(c.vkFocus));
    hotkeyLabels["fps"]        = ToUtf8(Dlss5Config::FormatKey(c.vkFps));
    hotkeyLabels["toggleVlss"] = ToUtf8(Dlss5Config::FormatKey(c.vkToggleVlss));
    hotkeyLabels["start"]      = ToUtf8(Dlss5Config::FormatKey(c.vkStart, c.modStart));
    data["hotkeyLabels"] = hotkeyLabels;

    data["statusText"] = ToUtf8(g_statusText);

    return data;
}

static void PushStateToJs()
{
    if (!g_mainWebView) return;
    json msg;
    msg["type"] = "state";
    msg["data"] = BuildStateJson();
    g_mainWebView->PostJson(FromUtf8(msg.dump()));
}

// ---------------------------------------------------------------------------
// Faz 3: "Tuşları Değiştir" sekmesi -- HotkeysWindow.cpp'deki PushHotkeysToJs
// ile AYNI mantik (bkz. plan mimari karari: kod tekrari > riskli paylasim).
// s_host'a degil g_mainWebView'e postalar.
// ---------------------------------------------------------------------------
static json BuildHotkeyLabelsJson()
{
    auto& cfg = ConfigManager::Get().Config();
    json labels;
    labels["0"] = ToUtf8(Dlss5Config::FormatKey(cfg.settingsVk, cfg.settingsMod));
    labels["2"] = ToUtf8(Dlss5Config::FormatKey(cfg.vkFocus));
    labels["3"] = ToUtf8(Dlss5Config::FormatKey(cfg.vkFps));
    labels["4"] = ToUtf8(Dlss5Config::FormatKey(cfg.vkToggleVlss));
    labels["5"] = ToUtf8(Dlss5Config::FormatKey(cfg.vkCalib));
    labels["6"] = ToUtf8(Dlss5Config::FormatKey(cfg.vkStart, cfg.modStart));
    labels["7"] = ToUtf8(Dlss5Config::FormatKey(cfg.vkDismissWarning));
    return labels;
}

static void PushHotkeysTabListToJs()
{
    if (!g_mainWebView) return;
    json msg;
    msg["type"] = "hotkeysList";
    msg["data"] = BuildHotkeyLabelsJson();
    g_mainWebView->PostJson(FromUtf8(msg.dump()));
}

// ---------------------------------------------------------------------------
// "Güncellemeler" sekmesi (bkz. UpdateChecker.h/.cpp) -- cmd/type "updates"
// onekiyle, diger sekmelerle (RTSS/Ön Ayarlar/Tuşlar) AYNI IPC deseni.
// ---------------------------------------------------------------------------
static void PushUpdatesStateToJs()
{
    if (!g_mainWebView) return;

    json data;
    data["currentVersion"] = ToUtf8(VLSS5_VERSION_STRING);
    data["autoCheck"]      = ConfigManager::Get().Config().autoCheckUpdates;
    data["checking"]       = g_updatesChecking;
    data["downloading"]    = g_updatesDownloading;
    data["hasUpdate"]      = g_updatesHasNewer;
    data["latestTag"]      = ToUtf8(g_updatesLatestTag);
    data["error"]          = ToUtf8(g_updatesLastError);

    json releases = json::array();
    for (auto& r : g_updatesCache)
    {
        json item;
        item["tag"]         = ToUtf8(r.tag);
        item["name"]        = ToUtf8(r.name);
        item["bodyHtml"]    = ToUtf8(r.bodyHtml);
        item["publishedAt"] = ToUtf8(r.publishedAt);
        item["htmlUrl"]     = ToUtf8(r.htmlUrl);
        item["prerelease"]  = r.prerelease;

        json assets = json::array();
        for (auto& a : r.assets)
        {
            json aj;
            aj["name"] = ToUtf8(a.name);
            aj["url"]  = ToUtf8(a.downloadUrl);
            aj["size"] = a.size;
            assets.push_back(aj);
        }
        item["assets"] = assets;

        releases.push_back(item);
    }
    data["releases"] = releases;

    json msg;
    msg["type"] = "updatesState";
    msg["data"] = data;
    g_mainWebView->PostJson(FromUtf8(msg.dump()));
}

// hwnd'ye WM_APP_UPDATES_FETCH_DONE ile geri donmek uzere arka planda GitHub
// Releases'i ceker. silentStartupCheck=true ise (uygulama acilisindaki otomatik
// kontrol) ve yeni bir surum bulunursa, sonuc geldiginde kullaniciya
// indirme/kurulum onay dialogu gosterilir (bkz. WM_APP_UPDATES_FETCH_DONE isleyicisi).
static void StartUpdatesCheck(HWND hwnd, bool silentStartupCheck)
{
    if (g_updatesChecking) return;
    g_updatesChecking = true;
    g_updatesLastError.clear();
    PushUpdatesStateToJs();

    std::thread([hwnd, silentStartupCheck]()
    {
        auto* result = new UpdateChecker::FetchResult(UpdateChecker::FetchLatestReleases(5));
        PostMessageW(hwnd, WM_APP_UPDATES_FETCH_DONE, silentStartupCheck ? 1 : 0, reinterpret_cast<LPARAM>(result));
    }).detach();
}

// Indirilen Inno Setup installer'ini SESSIZ/OTOMATIK kurulum bayraklariyla
// baslatir ve ardindan kendi surecimizi TEMIZ kapatir (bkz. installer/VLSS5.iss
// CloseApplications/RestartApplications -- installer, VLSS5.exe'yi Restart
// Manager ile bekleyip kurulum bitince KENDISI yeniden baslatir; burada ikinci
// bir baslatma YAPMIYORUZ).
static void LaunchSilentInstallAndExit(HWND hwnd, const std::wstring& installerPath)
{
    wchar_t exeDir[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exeDir, MAX_PATH);
    PathRemoveFileSpecW(exeDir);

    // VLSS5.exe zaten YUKSELTILMIS calisiyor (RequireAdministrator manifest,
    // bkz. VLSS5.vcxproj) -- CreateProcessW ile baslatilan bir alt surec,
    // installer'in KENDI manifesti admin istese BILE, ayrica bir UAC onayi
    // TETIKLEMEZ (bu davranis sadece ShellExecute'un "runas" / AppCompat
    // katmaninda olur). Bu yuzden ShellExecuteW degil, dogrudan CreateProcessW
    // kullaniyoruz -- boylece kurulum gercekten SESSIZ kalir.
    std::wstring cmdLine = L"\"" + installerPath + L"\""
        L" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP- /NOCANCEL"
        L" /CLOSEAPPLICATIONS /RESTARTAPPLICATIONS"
        L" /DIR=\"" + std::wstring(exeDir) + L"\"";

    std::vector<wchar_t> mutableCmd(cmdLine.begin(), cmdLine.end());
    mutableCmd.push_back(L'\0');

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE,
                         0, nullptr, nullptr, &si, &pi))
    {
        DLSS_Log("[Updates] Installer baslatilamadi (hata=%lu): %ls", GetLastError(), installerPath.c_str());
        return; // baslatilamadiysa kendimizi kapatmayalim -- kullanici elle deneyebilsin
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    DLSS_Log("[Updates] Installer sessizce baslatildi, uygulama kapatiliyor: %ls", installerPath.c_str());

    // WM_CLOSE -> varsayilan DefWindowProc DestroyWindow cagirir -> WM_DESTROY
    // (mevcut WebView2/hotkey/InputForwarder temizligi + PostQuitMessage) --
    // pencerenin X butonuna basilmasiyla AYNI, zaten var olan temiz kapanis yolu.
    PostMessageW(hwnd, WM_CLOSE, 0, 0);
}

// hwnd'ye WM_APP_UPDATES_DL_PROGRESS (tekrar tekrar) ve WM_APP_UPDATES_DL_DONE
// (bir kez) ile geri donmek uzere arka planda dosyayi %TEMP%'e indirir.
static void StartUpdatesDownload(HWND hwnd, const std::wstring& url, const std::wstring& assetName)
{
    if (g_updatesDownloading || url.empty()) return;
    g_updatesDownloading = true;
    PushUpdatesStateToJs();

    std::thread([hwnd, url, assetName]()
    {
        wchar_t tempDir[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, tempDir);
        std::wstring outPath = std::wstring(tempDir) + (assetName.empty() ? L"VLSS5_update.bin" : assetName);

        std::wstring error;
        bool ok = UpdateChecker::DownloadFile(url, outPath,
            [hwnd](const UpdateChecker::DownloadProgress& p)
            {
                auto* prog = new UpdateChecker::DownloadProgress(p);
                PostMessageW(hwnd, WM_APP_UPDATES_DL_PROGRESS, 0, reinterpret_cast<LPARAM>(prog));
            },
            error);

        auto* resultStr = new std::wstring(ok ? outPath : error);
        PostMessageW(hwnd, WM_APP_UPDATES_DL_DONE, ok ? 1 : 0, reinterpret_cast<LPARAM>(resultStr));
    }).detach();
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static void SetStatus(const wchar_t* text)
{
    g_statusText = text;
    PushStateToJs();
}

// HICON -> "data:image/png;base64,..." donusumu (hedef listesindeki pencere
// simgelerini HTML'e tasimak icin). GDI+ ile PNG'ye kodlanir (alpha kanali
// duzgun korunur), sonra elle base64'lenir. Basarisizlikta bos string doner --
// JS tarafi bunu jenerik bir glyph'e duser.
static int GetEncoderClsid(const WCHAR* mimeType, CLSID* clsid)
{
    UINT num = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0) return -1;

    std::vector<BYTE> buf(size);
    auto* codecInfo = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buf.data());
    Gdiplus::GetImageEncoders(num, size, codecInfo);

    for (UINT i = 0; i < num; ++i)
    {
        if (wcscmp(codecInfo[i].MimeType, mimeType) == 0)
        {
            *clsid = codecInfo[i].Clsid;
            return static_cast<int>(i);
        }
    }
    return -1;
}

static std::string Base64Encode(const std::vector<BYTE>& data)
{
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);

    size_t i = 0;
    while (i + 3 <= data.size())
    {
        unsigned v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out += tbl[(v >> 18) & 0x3F];
        out += tbl[(v >> 12) & 0x3F];
        out += tbl[(v >> 6) & 0x3F];
        out += tbl[v & 0x3F];
        i += 3;
    }
    size_t rem = data.size() - i;
    if (rem == 1)
    {
        unsigned v = data[i] << 16;
        out += tbl[(v >> 18) & 0x3F];
        out += tbl[(v >> 12) & 0x3F];
        out += "==";
    }
    else if (rem == 2)
    {
        unsigned v = (data[i] << 16) | (data[i + 1] << 8);
        out += tbl[(v >> 18) & 0x3F];
        out += tbl[(v >> 12) & 0x3F];
        out += tbl[(v >> 6) & 0x3F];
        out += "=";
    }
    return out;
}

// nvngx_dlssnr.dll surukle-birak yuklemesi icin ters yon -- JS tarafi dosya
// parcalarini base64 ile gonderiyor (bkz. "nvngxDropChunk"), burada cozulup
// diske yaziliyor. Base64Encode'un (yukarida, ikon data-URI'leri icin) TERSI.
static std::vector<BYTE> Base64Decode(const std::string& in)
{
    static int table[256];
    static bool tableInit = false;
    if (!tableInit)
    {
        std::fill(std::begin(table), std::end(table), -1);
        static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i) table[static_cast<unsigned char>(tbl[i])] = i;
        tableInit = true;
    }

    std::vector<BYTE> out;
    out.reserve((in.size() / 4) * 3);

    int val = 0, bits = -8;
    for (unsigned char c : in)
    {
        if (c == '=') break;
        if (table[c] == -1) continue; // satir sonu vb. guvenli sekilde atlanir

        val = (val << 6) + table[c];
        bits += 6;
        if (bits >= 0)
        {
            out.push_back(static_cast<BYTE>((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return out;
}

static std::string IconToDataUri(HICON hIcon)
{
    if (!hIcon) return {};

    Gdiplus::Bitmap bmp(hIcon);
    if (bmp.GetLastStatus() != Gdiplus::Ok) return {};

    IStream* stream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) || !stream)
        return {};

    CLSID pngClsid;
    if (GetEncoderClsid(L"image/png", &pngClsid) < 0)
    {
        stream->Release();
        return {};
    }

    bool ok = (bmp.Save(stream, &pngClsid) == Gdiplus::Ok);

    std::vector<BYTE> bytes;
    if (ok)
    {
        HGLOBAL hMem = nullptr;
        if (SUCCEEDED(GetHGlobalFromStream(stream, &hMem)) && hMem)
        {
            SIZE_T size = GlobalSize(hMem);
            void* pMem = GlobalLock(hMem);
            if (pMem && size > 0)
            {
                bytes.resize(size);
                memcpy(bytes.data(), pMem, size);
            }
            GlobalUnlock(hMem);
        }
    }
    stream->Release();

    if (bytes.empty()) return {};
    return "data:image/png;base64," + Base64Encode(bytes);
}

static void PopulateList(HWND menuHwnd)
{
    // "Tum Ekran" seçenekleri listenin başına eklenir, ardından açık pencereler gelir.
    g_windows = WindowEnumerator::GetMonitors();
    auto windows = WindowEnumerator::GetWindows(menuHwnd);
    g_windows.insert(g_windows.end(), windows.begin(), windows.end());

    g_windowIcons.clear();
    g_windowIcons.reserve(g_windows.size());
    for (auto& w : g_windows)
        g_windowIcons.push_back(IconToDataUri(w.icon));

    g_selectedTargetIndex = g_windows.empty() ? -1 : 0;
}

static bool ContainsCaseInsensitive(const std::wstring& str, const std::wstring& search)
{
    if (search.empty()) return true;
    if (str.length() < search.length()) return false;
    auto it = std::search(
        str.begin(), str.end(),
        search.begin(), search.end(),
        [](wchar_t ch1, wchar_t ch2) {
            return towlower(ch1) == towlower(ch2);
        });
    return (it != str.end());
}

static void PopulateGpuList(HWND /*hwnd*/)
{
    g_gpuList.clear();

    // Option 0: Auto (RTX Priority)
    GpuAdapterInfo autoOpt;
    autoOpt.name = L"Auto";
    autoOpt.displayName = L"⚡ Otomatik (RTX Öncelikli)";
    g_gpuList.push_back(autoOpt);

    ComPtr<IDXGIFactory1> factory;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
    {
        UINT i = 0;
        ComPtr<IDXGIAdapter1> adapter;
        while (factory->EnumAdapters1(i++, &adapter) != DXGI_ERROR_NOT_FOUND)
        {
            DXGI_ADAPTER_DESC1 desc{};
            adapter->GetDesc1(&desc);

            if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
            {
                GpuAdapterInfo info;
                info.name = desc.Description;
                SIZE_T vramMB = desc.DedicatedVideoMemory / (1024 * 1024);

                wchar_t disp[256];
                if (vramMB >= 1024)
                {
                    swprintf_s(disp, L"%ls (%.1f GB)", desc.Description, static_cast<float>(vramMB) / 1024.0f);
                }
                else if (vramMB > 0)
                {
                    swprintf_s(disp, L"%ls (%zu MB)", desc.Description, vramMB);
                }
                else
                {
                    swprintf_s(disp, L"%ls", desc.Description);
                }

                info.displayName = disp;
                g_gpuList.push_back(info);
            }
        }
    }

    if (g_gpuList.size() <= 1)
    {
        GpuAdapterInfo info;
        info.name = L"Varsayılan Grafik Kartı";
        info.displayName = L"Varsayılan Grafik Kartı";
        g_gpuList.push_back(info);
    }

    const std::wstring& savedGpu = ConfigManager::Get().Config().selectedGpu;
    int selectedIndex = 0; // Default to "⚡ Otomatik (RTX Öncelikli)"

    if (savedGpu.empty() || savedGpu == L"Auto")
    {
        selectedIndex = 0;
        // Check if an RTX card is available to log/verify
        bool hasRtx = false;
        for (size_t idx = 1; idx < g_gpuList.size(); ++idx)
        {
            if (ContainsCaseInsensitive(g_gpuList[idx].name, L"RTX"))
            {
                hasRtx = true;
                DLSS_Log("[Main] GPU Auto mode: RTX adapter ready (%ls)", g_gpuList[idx].name.c_str());
                break;
            }
        }
        if (!hasRtx && g_gpuList.size() > 1)
        {
            SetStatus(L"⚠ Sistemde RTX kart bulunamadı! DLSS5 için RTX gereklidir.");
        }

        if (savedGpu.empty())
        {
            ConfigManager::Get().Config().selectedGpu = L"Auto";
            ConfigManager::Get().Save();
        }
    }
    else
    {
        // Check if the saved GPU exists on this machine
        int matchedIdx = -1;
        for (size_t idx = 1; idx < g_gpuList.size(); ++idx)
        {
            if (ContainsCaseInsensitive(g_gpuList[idx].name, savedGpu))
            {
                matchedIdx = static_cast<int>(idx);
                break;
            }
        }

        if (matchedIdx != -1)
        {
            selectedIndex = matchedIdx;
        }
        else
        {
            // SELF-HEALING: The saved config references hardware not present on this machine!
            // Automatically find the best GPU on this system (prioritize RTX).
            int rtxIdx = -1;
            for (size_t idx = 1; idx < g_gpuList.size(); ++idx)
            {
                if (ContainsCaseInsensitive(g_gpuList[idx].name, L"RTX"))
                {
                    rtxIdx = static_cast<int>(idx);
                    break;
                }
            }

            if (rtxIdx != -1)
            {
                selectedIndex = rtxIdx;
                ConfigManager::Get().Config().selectedGpu = g_gpuList[rtxIdx].name;
                ConfigManager::Get().Save();

                std::wstring msg = L"Farklı donanım tespit edildi. Sisteminizdeki " + g_gpuList[rtxIdx].name + L" otomatik seçildi.";
                SetStatus(msg.c_str());
                DLSS_Log("[Main] Foreign GPU '%ls' not found. Self-healed config to: %ls", savedGpu.c_str(), g_gpuList[rtxIdx].name.c_str());
            }
            else
            {
                selectedIndex = 0; // Fallback to Auto
                ConfigManager::Get().Config().selectedGpu = L"Auto";
                ConfigManager::Get().Save();

                SetStatus(L"⚠ Farklı donanım tespit edildi ve RTX kart bulunamadı! (DLSS5 için RTX gereklidir)");
                DLSS_Log("[Main] Foreign GPU '%ls' not found. No RTX detected on this system.", savedGpu.c_str());
            }
        }
    }

    g_selectedGpuIndex = selectedIndex;

    // ---- DLSS hesaplama GPU'su listesi ----
    // Ayni g_gpuList'i indeksler; tek fark RTX olmayan kartlarin isaretlenmesi
    // (BuildStateJson'da "  — DLSS yok" etiketi eklenerek yapiliyor).
    {
        const std::wstring& savedDlssGpu = ConfigManager::Get().Config().dlssGpu;
        int dlssIndex = 0; // Otomatik (RTX Oncelikli)
        if (!savedDlssGpu.empty() && savedDlssGpu != L"Auto")
        {
            for (size_t idx = 1; idx < g_gpuList.size(); ++idx)
            {
                if (ContainsCaseInsensitive(g_gpuList[idx].name, savedDlssGpu))
                {
                    dlssIndex = static_cast<int>(idx);
                    break;
                }
            }

            if (dlssIndex == 0)
            {
                // Kayitli kart bu makinede yok: Otomatik'e dus ve kaydi duzelt.
                ConfigManager::Get().Config().dlssGpu = L"Auto";
                ConfigManager::Get().Save();
                DLSS_Log("[Main] Kayitli DLSS GPU'su '%ls' bulunamadi; Otomatik'e donuldu.", savedDlssGpu.c_str());
            }
        }

        g_selectedDlssGpuIndex = dlssIndex;
    }
}

// ---------------------------------------------------------------------------
// Hotkey & Capture Management
// ---------------------------------------------------------------------------
static DWORD g_lastHotkeyTick = 0;

static void RegisterAppHotkey(HWND hwnd)
{
    UnregisterHotKey(hwnd, ID_GLOBAL_HOTKEY);
    auto& cfg = ConfigManager::Get().Config();
    RegisterHotKey(hwnd, ID_GLOBAL_HOTKEY, cfg.modStart | MOD_NOREPEAT, cfg.vkStart);
}

static void UnregisterAppHotkey(HWND hwnd)
{
    UnregisterHotKey(hwnd, ID_GLOBAL_HOTKEY);
}

// Overlay su anda calisiyor mu? (BASLAT/DURDUR butonu ve cift-baslatma korumasi)
static bool IsOverlayRunning()
{
    return (g_app && g_app->GetState() == AppState::Capturing);
}

// BASLAT <-> DURDUR yazisi degistiginde butonu yeniden cizdir.
// Faz 2: artik native bir buton yok -- guncel state'i (isCapturing dahil)
// JS'e push ederek ayni etkiyi sagliyoruz.
static void RefreshStartButton()
{
    PushStateToJs();
}

static bool StartCaptureWithTarget(HWND hwnd, HWND target)
{
    // Ic ice Run() dongusune karsi son savunma hatti.
    if (IsOverlayRunning())
        return false;

    if (!IsWindow(target))
    {
        SetStatus(L"Seçilen pencere artık açık değil — Yenile'ye basın.");
        return false;
    }

    // Insert menusu (VARSAYILAN VLSS5 AYARLARI dahil) yakalama baslarken KESIN
    // KAPATILIR -- App::StartOverlayCommon'da da ayni cagri var, burada en
    // erken noktada (ShowWindow/EnableWindow'dan bile once) tekrarlanmasi
    // sirali/zamanlama sorunlarina karsi ek guvence.
    SettingsWindow::Hide();

    // Overlay calisirken ana pencere GIZLENMEZ, sadece simge durumuna kucultulur.
    // SW_HIDE taskbar kaydini da siliyordu ve kullanici programi kapatamiyordu.
    ShowWindow(hwnd, SW_MINIMIZE);
    RefreshStartButton();

    // "Varsayılan VLSS5 Ayarları" penceresi yakalama surerken ACILAMAZ -- artik
    // JS tarafinda "settingsBtnEnabled" (RefreshStartButton'un pushladigi state
    // icinde, isCapturing'den turetilir) ile gorsel olarak devre disi birakilir.

    if (!g_app->StartOverlay(hwnd, target, g_vsyncEnabled, g_dlssEnabled, g_fpsEnabled, g_fullscreenStretch))
    {
        ShowWindow(hwnd, SW_RESTORE);
        RefreshStartButton();
        SetForegroundWindow(hwnd);
        SetStatus(L"Overlay başlatılamadı. Pencereyi kontrol edin.");
        return false;
    }

    // g_app->GetState() artik Capturing -- JS'e it ki BASLAT butonu DURDUR'a
    // donsun (pencere minimize olmadan once kisa bir an gorunur olabilir, ve
    // Run() dongusu icinde ayni webview yine mesaj pompaliyor).
    RefreshStartButton();

    // Blocking render loop
    g_app->Run();

    // Prevent immediate hotkey re-trigger when returning from overlay
    g_lastHotkeyTick = GetTickCount();
    MSG flushMsg = {};
    while (PeekMessageW(&flushMsg, nullptr, WM_HOTKEY, WM_HOTKEY, PM_REMOVE)) {}

    // Wait until key is released (up to 300ms) so releasing Alt+S doesn't trigger start
    DWORD waitStart = GetTickCount();
    while (InputForwarder::IsStopKeyDown() && (GetTickCount() - waitStart < 300))
    {
        Sleep(10);
    }

    // When returned, restore main window
    ShowWindow(hwnd, SW_RESTORE);
    RefreshStartButton();
    SetForegroundWindow(hwnd);
    BringWindowToTop(hwnd);
    PopulateList(hwnd);

    std::wstring msg = L"Overlay durduruldu.  [";
    msg += InputForwarder::Config().FormatDisplay();
    msg += L"] veya BAŞLAT ile tekrar açabilirsiniz.";
    SetStatus(msg.c_str());
    return true;
}

static bool StartCaptureWithMonitor(HWND hwnd, HMONITOR monitor)
{
    // Ic ice Run() dongusune karsi son savunma hatti.
    if (IsOverlayRunning())
        return false;

    if (!monitor)
        return false;

    // Insert menusu (VARSAYILAN VLSS5 AYARLARI dahil) yakalama baslarken KESIN
    // KAPATILIR -- bkz. StartCaptureWithTarget'taki ayni yorum.
    SettingsWindow::Hide();

    // Overlay calisirken ana pencere GIZLENMEZ, sadece simge durumuna kucultulur.
    ShowWindow(hwnd, SW_MINIMIZE);
    RefreshStartButton();

    // "Varsayılan VLSS5 Ayarları" penceresi yakalama surerken ACILAMAZ -- JS
    // tarafinda "settingsBtnEnabled" ile gorsel olarak devre disi birakilir.

    if (!g_app->StartOverlayDesktop(hwnd, monitor, g_vsyncEnabled, g_dlssEnabled, g_fpsEnabled))
    {
        ShowWindow(hwnd, SW_RESTORE);
        RefreshStartButton();
        SetForegroundWindow(hwnd);
        SetStatus(L"Overlay başlatılamadı. Ekranı kontrol edin.");
        return false;
    }

    // g_app->GetState() artik Capturing -- JS'e it ki BASLAT butonu DURDUR'a
    // donsun (bkz. StartCaptureWithTarget'taki ayni aciklama).
    RefreshStartButton();

    // Blocking render loop
    g_app->Run();

    // Prevent immediate hotkey re-trigger when returning from overlay
    g_lastHotkeyTick = GetTickCount();
    MSG flushMsg = {};
    while (PeekMessageW(&flushMsg, nullptr, WM_HOTKEY, WM_HOTKEY, PM_REMOVE)) {}

    // Wait until key is released (up to 300ms) so releasing Alt+S doesn't trigger start
    DWORD waitStart = GetTickCount();
    while (InputForwarder::IsStopKeyDown() && (GetTickCount() - waitStart < 300))
    {
        Sleep(10);
    }

    // When returned, restore main window
    ShowWindow(hwnd, SW_RESTORE);
    RefreshStartButton();
    SetForegroundWindow(hwnd);
    BringWindowToTop(hwnd);
    PopulateList(hwnd);

    std::wstring msg = L"Overlay durduruldu.  [";
    msg += InputForwarder::Config().FormatDisplay();
    msg += L"] veya BAŞLAT ile tekrar açabilirsiniz.";
    SetStatus(msg.c_str());
    return true;
}

static void TriggerCaptureToggle(HWND hwnd)
{
    DWORD now = GetTickCount();
    if (now - g_lastHotkeyTick < 400) // 400ms debounce
        return;
    g_lastHotkeyTick = now;

    if (g_app && g_app->GetState() == AppState::Capturing)
    {
        // Currently capturing -> Stop it!
        g_app->RequestStop();
        return;
    }

    // Currently in Menu -> Start it!
    HWND target = nullptr;
    HWND fg = GetForegroundWindow();

    // 1. Check if the foreground window is an active application/game window (not our menu, not shell)
    if (fg && fg != hwnd && IsWindow(fg) && IsWindowVisible(fg))
    {
        wchar_t cls[64] = {};
        GetClassNameW(fg, cls, _countof(cls));

        // Kendi surecimize ait HERHANGI bir pencere (VLSS5 Ayarlari, RTSS
        // Ayarlari, Tuslar, On Ayarlar vs.) asla yakalama hedefi olmamali --
        // aksi halde ALT+S o pencerenin uzerine bir overlay acar, pencere hic
        // kapanmaz, sadece arkada kalmis gibi gorunur (kullanici geri bildirimi).
        DWORD fgPid = 0;
        GetWindowThreadProcessId(fg, &fgPid);
        bool isOwnProcessWindow = (fgPid == GetCurrentProcessId());

        if (!isOwnProcessWindow &&
            wcscmp(cls, L"Progman") != 0 &&
            wcscmp(cls, L"WorkerW") != 0 &&
            wcscmp(cls, L"Shell_TrayWnd") != 0 &&
            wcscmp(cls, L"Shell_SecondaryTrayWnd") != 0)
        {
            target = fg;
        }
    }

    // 2. If foreground was the menu window itself or a system window, use the selected entry in the list --
    // ama SADECE gercek bir pencere ise. Masaustu/monitor yakalamasi (targetMonitor) kisayoldan (Alt+S)
    // ASLA baslatilamaz -- kullanici bunu sadece menuden, BAŞLAT butonuyla secip yapabilmeli
    // (bkz. IDC_BTN_START isleyicisi). Aksi halde hotkey bazen dogrudan masaustune render uyguluyordu.
    if (!target)
    {
        int sel = g_selectedTargetIndex;
        if (sel >= 0 && sel < static_cast<int>(g_windows.size()) && !g_windows[sel].monitor)
        {
            target = g_windows[sel].hwnd;
        }
        else if (!g_windows.empty() && !g_windows[0].monitor)
        {
            target = g_windows[0].hwnd;
        }
    }

    if (target && IsWindow(target))
    {
        // Highlight in list if present
        for (size_t i = 0; i < g_windows.size(); ++i)
        {
            if (g_windows[i].hwnd == target)
            {
                g_selectedTargetIndex = static_cast<int>(i);
                PushStateToJs();
                break;
            }
        }

        StartCaptureWithTarget(hwnd, target);
    }
    else
    {
        SetStatus(L"Yakalanacak pencere bulunamadı. Masaüstü (monitör) yakalaması yalnızca menüden BAŞLAT ile seçilebilir.");
    }
}

// ---------------------------------------------------------------------------
// JS -> Native mesaj koprusu (WebViewHost::MessageHandler)
// ---------------------------------------------------------------------------
static void OnMainWebMessage(const std::wstring& jsonStr)
{
    json msg;
    try { msg = json::parse(ToUtf8(jsonStr)); }
    catch (...) { return; }

    // Son savunma hatti: SettingsWindow/RtssWindow/HotkeysWindow/PresetsWindow'daki
    // ayni desen -- tum dispatch try/catch icinde, sadece json::parse degil.
    // Bir istisna bu COM callback sinirini asarsa std::terminate cagrilip TUM
    // uygulama sessizce cokebilir (bkz. Faz 1'de bulunan gercek hata).
    try
    {
        std::string cmd = msg.value("cmd", "");
        HWND hwnd = g_mainHwnd;

        if (cmd == "getState")
        {
            PushStateToJs();
        }
        else if (cmd == "refresh")
        {
            PopulateList(hwnd);
            PopulateGpuList(hwnd);
            SetStatus(L"Pencere ve GPU listesi güncellendi.");
        }
        else if (cmd == "selectTarget")
        {
            int idx = msg.value("id", -1);
            if (idx >= 0 && idx < static_cast<int>(g_windows.size()))
            {
                g_selectedTargetIndex = idx;
                PushStateToJs();
            }
        }
        else if (cmd == "setVsync")
        {
            g_vsyncEnabled = msg.value("value", g_vsyncEnabled);
            if (g_vsyncEnabled)
                SetStatus(L"VSync etkin: Kareler monitör yenileme hızına kilitlenecek.");
            else
                SetStatus(L"VSync kapalı: Sınırsız kare hızı & en düşük gecikme.");
        }
        else if (cmd == "setFps")
        {
            g_fpsEnabled = msg.value("value", g_fpsEnabled);
            PushStateToJs();
        }
        else if (cmd == "setDlss")
        {
            g_dlssEnabled = msg.value("value", g_dlssEnabled);
            if (g_dlssEnabled)
                SetStatus(L"VLSS5 etkin: Nöral iyileştirme devrede.");
            else
                SetStatus(L"VLSS5 kapalı: Standart doğrudan görüntü modu.");
        }
        else if (cmd == "setFullscreen")
        {
            g_fullscreenStretch = msg.value("value", g_fullscreenStretch);
            ConfigManager::Get().Config().fullscreenStretch = g_fullscreenStretch;
            ConfigManager::Get().Save();

            if (g_fullscreenStretch)
                SetStatus(L"Tam Ekran Yap etkin: Pencere, monitörün tamamına gerdirilecek.");
            else
                SetStatus(L"Tam Ekran Yap kapalı: Overlay pencerenin kendi boyutunda kalacak.");
        }
        else if (cmd == "setGpu")
        {
            int sel = msg.value("index", -1);
            if (sel >= 0 && sel < static_cast<int>(g_gpuList.size()))
            {
                g_selectedGpuIndex = sel;
                const std::wstring& chosenName = g_gpuList[sel].name;
                ConfigManager::Get().Config().selectedGpu = chosenName;
                ConfigManager::Get().Save();

                if (g_app)
                {
                    g_app->SetPreferredGpu(chosenName);
                }

                std::wstring statusMsg;
                if (chosenName == L"Auto")
                    statusMsg = L"Ekran/yakalama GPU: Otomatik (RTX Öncelikli)";
                else
                    statusMsg = L"Ekran/yakalama GPU: " + g_gpuList[sel].displayName;
                SetStatus(statusMsg.c_str());
                DLSS_Log("[Main] GPU selection changed to: %ls", chosenName.c_str());
            }
        }
        else if (cmd == "setDlssGpu")
        {
            int sel = msg.value("index", -1);
            if (sel >= 0 && sel < static_cast<int>(g_gpuList.size()))
            {
                g_selectedDlssGpuIndex = sel;
                const std::wstring& chosenName = g_gpuList[sel].name;
                ConfigManager::Get().Config().dlssGpu = chosenName;
                ConfigManager::Get().Save();

                // Cihazlar oturum basinda kuruluyor; degisiklik bir sonraki
                // BASLAT'ta etkili olur.
                std::wstring statusMsg;
                if (chosenName == L"Auto")
                {
                    statusMsg = L"DLSS hesaplama GPU: Otomatik (RTX Öncelikli)";
                }
                else if (!ContainsCaseInsensitive(chosenName, L"RTX"))
                {
                    statusMsg = L"⚠ " + g_gpuList[sel].displayName + L" DLSS5 çalıştıramaz! RTX bir kart seçin.";
                }
                else
                {
                    statusMsg = L"DLSS hesaplama GPU: " + g_gpuList[sel].displayName;
                }

                // Yakalama kartindan farkliysa kare basina PCIe transferi olur.
                const std::wstring& captureGpu = ConfigManager::Get().Config().selectedGpu;
                if (chosenName != captureGpu)
                    statusMsg += L"  (ayrı kart — yeniden başlatın)";

                SetStatus(statusMsg.c_str());
                DLSS_Log("[Main] DLSS hesaplama GPU'su: %ls", chosenName.c_str());
            }
        }
        else if (cmd == "setCaptureBackend")
        {
            // UI'da kilitli/devre disi -- JS normalde bunu hic gondermez, yine de
            // savunmaci olarak, eski WM_COMMAND davranisiyla ayni sekilde islenir.
            int sel = msg.value("index", 0);
            ConfigManager::Get().Config().captureBackend = sel;
            ConfigManager::Get().Save();

            SetStatus((sel == 1)
                ? L"Yakalama yöntemi: DXGI Desktop Duplication (Deneysel) — bir sonraki BAŞLAT'ta etkili olur"
                : L"Yakalama yöntemi: WGC (Varsayılan) — bir sonraki BAŞLAT'ta etkili olur");
            DLSS_Log("[Main] Yakalama yontemi degisti: %s", (sel == 1) ? "DXGI Desktop Duplication" : "WGC");
        }
        else if (cmd == "startStop")
        {
            // Zaten calisiyorsa yeniden BASLATMA -- ic ice Run() dongusu acilirdi.
            if (IsOverlayRunning())
            {
                g_app->RequestStop();
                SetStatus(L"Overlay durduruluyor...");
            }
            else if (!NvngxDlssnrExists())
            {
                // Bkz. dllStatusBar (Ana Sayfa) -- JS tarafi zaten BASLAT'i
                // devre disi birakiyor, burasi sadece savunmaci ikinci kontrol.
                SetStatus(L"nvngx_dlssnr.dll bulunamadı -- lütfen üstteki alana sürükleyip bırakın.");
            }
            else
            {
                int sel = msg.value("targetId", g_selectedTargetIndex);
                if (sel < 0 || sel >= static_cast<int>(g_windows.size()))
                {
                    SetStatus(L"Lütfen listeden bir pencere seçin.");
                }
                else
                {
                    g_selectedTargetIndex = sel;
                    if (g_windows[sel].monitor)
                        StartCaptureWithMonitor(hwnd, g_windows[sel].monitor);
                    else
                        StartCaptureWithTarget(hwnd, g_windows[sel].hwnd);
                }
            }
        }
        else if (cmd == "openSettings")
        {
            // "Varsayılan VLSS5 Ayarları" penceresi SADECE yakalama calismiyorken
            // acilabilir -- bkz. eski IDC_BTN_DLSS_SETTINGS yorumu (mukerrer/
            // belirsiz on ayar duzenleme durumunu onlemek icin).
            //
            // KRITIK: SettingsWindow::Toggle() (ilk acilista) yeni bir WebView2
            // controller'i (CreatePopup) OLUSTURUR. Bunu burada, ana pencerenin
            // KENDI WebView2 WebMessageReceived callback'i ICINDEN senkron
            // cagirmak WebView2'nin ic-ice (reentrant) async COM cagrisini hic
            // TAMAMLAMAMASINA yol aciyor -- yeni pencere 10 saniye timeout'a
            // girip sonunda dumduz SIYAH kaliyor (controller hic olusmuyor).
            // Cozum: gercek cagriyi WM_APP+4 ile ertele, boylece bu callback'in
            // cagri yigininin TAMAMEN disina cikip normal mesaj dongusune
            // dondukten sonra calissin.
            if (!IsOverlayRunning())
                PostMessageW(hwnd, WM_APP + 4, 0, 0);
        }
        else if (cmd == "openRtss")
        {
            PostMessageW(hwnd, WM_APP + 4, 1, 0);
        }
        else if (cmd == "openHotkeys")
        {
            PostMessageW(hwnd, WM_APP + 4, 2, 0);
        }
        else if (cmd == "openPresets")
        {
            PostMessageW(hwnd, WM_APP + 4, 3, 0);
        }
        else if (cmd == "help")
        {
            std::string topic = msg.value("topic", "");
            if (topic == "gpu")
            {
                MessageBoxW(
                    hwnd,
                    L"Bu kart ekranı yakalar ve overlay'i sunar.\n\n"
                    L"Monitörünüzü hangi kart sürüyorsa o seçilmelidir: tam ekran sunum, "
                    L"ekranı süren kartta olmak zorundadır.\n\n"
                    L"Sinir ağını başka bir karta vermek isterseniz bir alttaki "
                    L"\"DLSS Hesaplama GPU\" ayarını kullanın.",
                    L"VLSS5 — Ekran / Yakalama GPU",
                    MB_ICONINFORMATION | MB_OK);
            }
            else if (topic == "dlssGpu")
            {
                MessageBoxW(
                    hwnd,
                    L"Sinir ağının (DLSS5) koşacağı kart.\n\n"
                    L"RTX bir kart gereklidir; AMD ve Intel kartlarda çalışmaz.\n\n"
                    L"Yakalama kartıyla AYNI seçilirse en düşük gecikmeyi alırsınız.\n\n"
                    L"FARKLI seçilirse her kare (giriş + hareket vektörü + çıkış) PCIe "
                    L"üzerinden taşınır. Bu, monitörü iGPU sürüyorsa mantıklıdır; iki güçlü "
                    L"kart arasında genellikle zarar eder.\n\n"
                    L"Değişiklik bir sonraki BAŞLAT'ta etkili olur.",
                    L"VLSS5 — DLSS Hesaplama GPU",
                    MB_ICONINFORMATION | MB_OK);
            }
            else if (topic == "captureBackend")
            {
                MessageBoxW(
                    hwnd,
                    L"WGC (Windows.Graphics.Capture): Varsayılan. Her durumda çalışır.\n\n"
                    L"DXGI Desktop Duplication (Deneysel): Pencere modunda WGC'nin bazen bulanıklaştırdığı "
                    L"görüntüyü keskinleştirir. Sadece ekranı süren kartla aynı adaptörde çalışır; "
                    L"başlatılamazsa VLSS5 otomatik olarak WGC'ye döner (log'da görürsünüz).\n\n"
                    L"Pencere üzerine gelen başka bir pencere (bildirim, alt-tab vb.) o kareyi atlatır, "
                    L"eski görüntü kalır -- kirli kare gösterilmez.\n\n"
                    L"Değişiklik bir sonraki BAŞLAT'ta etkili olur.",
                    L"VLSS5 — Yakalama Yöntemi",
                    MB_ICONINFORMATION | MB_OK);
            }
        }
        // -------------------------------------------------------------
        // Faz 3: "Ayarlar" sekmesi (bkz. SettingsWindow::OnWebMessage'daki
        // "getConfig"/"setConfig"/"listPresets"/"loadPreset" dallariyla AYNI
        // mantik). Bu sekme SADECE yakalama calismiyorken kullanilabilir
        // (bkz. plan section 4 notu) -- bu yuzden SettingsWindowHasActiveTarget
        // dalina hic girmeden HER ZAMAN ConfigManager::Get().Save() kullanilir.
        // -------------------------------------------------------------
        else if (cmd == "settingsGetConfig")
        {
            const auto& cfg = ConfigManager::Get().Config();
            json data;
            data["style"]           = cfg.style;
            data["preset"]          = cfg.preset;
            data["intensity"]       = cfg.intensity;
            data["boostFactor"]     = cfg.boostFactor;
            data["localStructure"]  = cfg.localStructure;
            data["localTone"]       = cfg.localTone;
            data["skinStructure"]   = cfg.skinStructure;
            data["resolutionScale"] = cfg.resolutionScale;
            data["passCount"]       = cfg.passCount;
            data["passFalloff"]     = cfg.passFalloff;
            data["useAutoMask"]     = cfg.useAutoMask;
            data["opticalFlow"]     = cfg.opticalFlow;
            data["splitScreen"]     = cfg.splitScreen;
            data["splitPos"]        = cfg.splitPos;

            json out;
            out["type"] = "settingsConfig";
            out["data"] = data;
            if (g_mainWebView) g_mainWebView->PostJson(FromUtf8(out.dump()));
        }
        else if (cmd == "settingsListPresets")
        {
            auto entries = PresetsManager::Get().ListPresets();
            json arr = json::array();
            for (auto& e : entries)
            {
                json item;
                item["folder"]      = ToUtf8(e.folderName);
                item["displayName"] = ToUtf8(e.displayName);
                arr.push_back(item);
            }
            json out;
            out["type"] = "settingsPresetList";
            out["data"] = arr;
            if (g_mainWebView) g_mainWebView->PostJson(FromUtf8(out.dump()));
        }
        else if (cmd == "settingsSetConfig")
        {
            // KRITIK: yakalama calisirken bu sekme JS tarafinda GIZLENMEZ/kapatilmaz
            // (pencere sadece minimize edilir, kullanici taskbar'dan geri getirip
            // yine de dokunabilir). ConfigManager::Get().Config() render motorunun
            // HER KARE dogrudan okudugu TEK PAYLASIMLI struct -- buraya yazmak,
            // "varsayilan" niyetiyle yapilsa bile o an yakalanan hedefin (kendi
            // kayitli on ayari olsa DAHI) canli goruntusunu aninda degistiriyordu.
            // Yakalama surerken bu sekmeden gelen degisiklikleri sessizce reddet.
            if (IsOverlayRunning())
            {
                SetStatus(L"Yakalama sürerken Varsayılan Ayarlar değiştirilemez.");
                return;
            }
            if (msg.contains("data"))
            {
                auto& d = msg["data"];
                auto& cfg = ConfigManager::Get().Config();

                cfg.style           = d.value("style", cfg.style);
                cfg.preset          = d.value("preset", cfg.preset);
                cfg.intensity       = d.value("intensity", cfg.intensity);
                cfg.boostFactor     = std::clamp(d.value("boostFactor", cfg.boostFactor), 1.0f, 2.5f);
                cfg.localStructure  = d.value("localStructure", cfg.localStructure);
                cfg.localTone       = d.value("localTone", cfg.localTone);
                cfg.skinStructure   = d.value("skinStructure", cfg.skinStructure);
                if (cfg.skinStructure <= -0.99f) cfg.skinStructure = -1.0f;
                cfg.resolutionScale = d.value("resolutionScale", cfg.resolutionScale);
                cfg.passCount       = std::clamp(d.value("passCount", cfg.passCount), 1, 4);
                cfg.passFalloff     = std::clamp(d.value("passFalloff", cfg.passFalloff), 0.25f, 1.0f);
                cfg.useAutoMask     = d.value("useAutoMask", cfg.useAutoMask);
                cfg.opticalFlow     = d.value("opticalFlow", cfg.opticalFlow);
                cfg.splitScreen     = d.value("splitScreen", cfg.splitScreen);
                cfg.splitPos        = std::clamp(d.value("splitPos", cfg.splitPos), 0.0f, 1.0f);

                // Bu sekme sadece yakalama calismiyorken kullanilabilir --
                // SettingsWindow'daki "aktif hedef on ayarina yaz" dalina
                // hic gerek yok, her zaman global config'e kaydedilir.
                ConfigManager::Get().Save();
            }
        }
        else if (cmd == "settingsLoadPreset")
        {
            // Ayni gerekce: bkz. settingsSetConfig -- yakalama surerken burasi
            // canli ConfigManager struct'ina yazip aktif hedefi bozmasin.
            if (IsOverlayRunning())
            {
                SetStatus(L"Yakalama sürerken Varsayılan Ayarlar değiştirilemez.");
                return;
            }
            std::string folder = msg.value("folder", "");
            if (!folder.empty())
            {
                Dlss5Config loaded;
                if (PresetsManager::Get().LoadPresetConfig(FromUtf8(folder), loaded))
                {
                    auto& cfg = ConfigManager::Get().Config();
                    cfg.style           = loaded.style;
                    cfg.preset          = loaded.preset;
                    cfg.intensity       = loaded.intensity;
                    cfg.boostFactor     = loaded.boostFactor;
                    cfg.localStructure  = loaded.localStructure;
                    cfg.localTone       = loaded.localTone;
                    cfg.skinStructure   = loaded.skinStructure;
                    cfg.resolutionScale = loaded.resolutionScale;
                    cfg.passCount       = loaded.passCount;
                    cfg.passFalloff     = loaded.passFalloff;
                    cfg.useAutoMask     = loaded.useAutoMask;
                    cfg.opticalFlow     = loaded.opticalFlow;
                    cfg.splitScreen     = loaded.splitScreen;
                    cfg.splitPos        = loaded.splitPos;

                    ConfigManager::Get().Save();

                    json data;
                    data["style"]           = cfg.style;
                    data["preset"]          = cfg.preset;
                    data["intensity"]       = cfg.intensity;
                    data["boostFactor"]     = cfg.boostFactor;
                    data["localStructure"]  = cfg.localStructure;
                    data["localTone"]       = cfg.localTone;
                    data["skinStructure"]   = cfg.skinStructure;
                    data["resolutionScale"] = cfg.resolutionScale;
                    data["passCount"]       = cfg.passCount;
                    data["passFalloff"]     = cfg.passFalloff;
                    data["useAutoMask"]     = cfg.useAutoMask;
                    data["opticalFlow"]     = cfg.opticalFlow;
                    data["splitScreen"]     = cfg.splitScreen;
                    data["splitPos"]        = cfg.splitPos;

                    json out;
                    out["type"] = "settingsConfig";
                    out["data"] = data;
                    if (g_mainWebView) g_mainWebView->PostJson(FromUtf8(out.dump()));
                }
            }
        }
        // -------------------------------------------------------------
        // Faz 3: "RTSS Ayarları" sekmesi (bkz. RtssWindow::DispatchMessage
        // ile AYNI mantik).
        // -------------------------------------------------------------
        else if (cmd == "rtssGetDir")
        {
            std::wstring dir = ConfigManager::Get().Config().rtssDirectory;
            std::wstring shown = dir.empty() ? (L"(Varsayılan) " + RTSSManager::Get().GetProfilesDir()) : dir;

            wchar_t buf[440] = {};
            wcsncpy_s(buf, shown.c_str(), _TRUNCATE);
            PathCompactPathExW(buf, shown.c_str(), 70, 0);

            json out;
            out["type"] = "rtssDir";
            out["data"] = ToUtf8(buf);
            if (g_mainWebView) g_mainWebView->PostJson(FromUtf8(out.dump()));
        }
        else if (cmd == "rtssListProfiles")
        {
            json arr = json::array();
            for (const auto& prof : RTSSManager::Get().GetProfiles())
                arr.push_back(ToUtf8(prof));

            json out;
            out["type"] = "rtssProfileList";
            out["data"] = arr;
            if (g_mainWebView) g_mainWebView->PostJson(FromUtf8(out.dump()));
        }
        else if (cmd == "rtssBrowseFolder")
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

                            // rtssListProfiles + rtssGetDir esdegeri: guncel
                            // klasoru/profilleri hemen JS'e geri bas.
                            json arr = json::array();
                            for (const auto& prof : RTSSManager::Get().GetProfiles())
                                arr.push_back(ToUtf8(prof));
                            json outProfiles;
                            outProfiles["type"] = "rtssProfileList";
                            outProfiles["data"] = arr;
                            if (g_mainWebView) g_mainWebView->PostJson(FromUtf8(outProfiles.dump()));

                            std::wstring dir = ConfigManager::Get().Config().rtssDirectory;
                            wchar_t buf[440] = {};
                            wcsncpy_s(buf, dir.c_str(), _TRUNCATE);
                            PathCompactPathExW(buf, dir.c_str(), 70, 0);
                            json outDir;
                            outDir["type"] = "rtssDir";
                            outDir["data"] = ToUtf8(buf);
                            if (g_mainWebView) g_mainWebView->PostJson(FromUtf8(outDir.dump()));
                        }
                        psi->Release();
                    }
                }
                pfd->Release();
            }
        }
        else if (cmd == "rtssDeleteProfile")
        {
            std::wstring name = FromUtf8(msg.value("name", std::string()));
            if (!name.empty())
            {
                std::wstring dispMsg = L"\"" + name + L"\" profili silinsin mi?";
                if (ConfirmDialog::AskYesNo(hwnd, L"VLSS5 - Profili Sil",
                        L"Bu RTSS profilini silmek istediğinizden emin misiniz?", dispMsg.c_str(),
                        L"Evet", L"Hayır", /*defaultIsYes*/false, /*warningIcon*/true))
                {
                    std::wstring cfgPath = RTSSManager::Get().GetProfilesDir() + L"\\" + name + L".cfg";
                    DeleteFileW(cfgPath.c_str());
                    RTSSManager::Get().NotifyRTSS();

                    json arr = json::array();
                    for (const auto& prof : RTSSManager::Get().GetProfiles())
                        arr.push_back(ToUtf8(prof));
                    json out;
                    out["type"] = "rtssProfileList";
                    out["data"] = arr;
                    if (g_mainWebView) g_mainWebView->PostJson(FromUtf8(out.dump()));
                }
            }
        }
        // -------------------------------------------------------------
        // Faz 3: "Ön Ayarlar" sekmesi (bkz. PresetsWindow::DispatchMessage
        // ile AYNI mantik).
        // -------------------------------------------------------------
        else if (cmd == "presetsList")
        {
            g_presetsTabCache = PresetsManager::Get().ListPresets();
            json arr = json::array();
            for (auto& e : g_presetsTabCache)
            {
                json item;
                item["folder"]      = ToUtf8(e.folderName);
                item["displayName"] = ToUtf8(e.displayName);
                arr.push_back(item);
            }
            json out;
            out["type"] = "presetsPresetList";
            out["data"] = arr;
            if (g_mainWebView) g_mainWebView->PostJson(FromUtf8(out.dump()));
        }
        else if (cmd == "presetsOpenFolder")
        {
            std::wstring dir = PresetsManager::Get().GetPresetsRootDir();
            ShellExecuteW(hwnd, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        else if (cmd == "presetsDeletePreset")
        {
            std::wstring folder = FromUtf8(msg.value("folder", std::string()));
            if (!folder.empty())
            {
                auto it = std::find_if(g_presetsTabCache.begin(), g_presetsTabCache.end(),
                    [&](const PresetEntry& e) { return e.folderName == folder; });
                std::wstring displayName = (it != g_presetsTabCache.end()) ? it->displayName : folder;

                std::wstring dispMsg = L"\"" + displayName + L"\" silinsin mi?";
                if (ConfirmDialog::AskYesNo(hwnd, L"VLSS5 - Ön Ayarı Sil",
                        L"Bu ön ayarı silmek istediğinizden emin misiniz?", dispMsg.c_str(),
                        L"Evet", L"Hayır", /*defaultIsYes*/false, /*warningIcon*/true))
                {
                    PresetsManager::Get().DeletePreset(folder);

                    g_presetsTabCache = PresetsManager::Get().ListPresets();
                    json arr = json::array();
                    for (auto& e : g_presetsTabCache)
                    {
                        json item;
                        item["folder"]      = ToUtf8(e.folderName);
                        item["displayName"] = ToUtf8(e.displayName);
                        arr.push_back(item);
                    }
                    json out;
                    out["type"] = "presetsPresetList";
                    out["data"] = arr;
                    if (g_mainWebView) g_mainWebView->PostJson(FromUtf8(out.dump()));
                }
            }
        }
        // -------------------------------------------------------------
        // Faz 3: "Tuşları Değiştir" sekmesi -- WH_KEYBOARD_LL hook mekanizmasi
        // HotkeysWindow'un KENDI statik uyeleri uzerinde calisiyor; burada
        // TEKRAR YAZILMAZ, dogrudan (artik public olan) StartKeybindCapture
        // cagrilir. Sonuc HotkeysWindow::SetExternalKeyCapturedCallback ile
        // WinMain'de bir kez baglanan geri cagirma uzerinden g_mainWebView'e
        // asenkron olarak (RebindKeyboardProc -> EndKeybindCapture) geri doner.
        // -------------------------------------------------------------
        else if (cmd == "hotkeysGetHotkeys")
        {
            PushHotkeysTabListToJs();
        }
        else if (cmd == "hotkeysStartCapture")
        {
            int id = msg.value("hotkeyId", -1);
            HotkeysWindow::StartKeybindCapture(id);
        }
        // -------------------------------------------------------------
        // "Güncellemeler" sekmesi (bkz. UpdateChecker.h/.cpp, PushUpdatesStateToJs).
        // -------------------------------------------------------------
        else if (cmd == "updatesGetState")
        {
            PushUpdatesStateToJs();
        }
        else if (cmd == "updatesSetAutoCheck")
        {
            bool value = msg.value("value", true);
            ConfigManager::Get().Config().autoCheckUpdates = value;
            ConfigManager::Get().Save();
            PushUpdatesStateToJs();
        }
        else if (cmd == "updatesCheckNow")
        {
            StartUpdatesCheck(hwnd, /*silentStartupCheck*/false);
        }
        else if (cmd == "updatesDownload")
        {
            std::wstring url  = FromUtf8(msg.value("url", std::string()));
            std::wstring name = FromUtf8(msg.value("name", std::string()));
            StartUpdatesDownload(hwnd, url, name);
        }
        // -------------------------------------------------------------
        // nvngx_dlssnr.dll surukle-birak yuklemesi (bkz. Ana Sayfa dllStatusBar,
        // web/main/app.js uploadNvngxDlssnr). WebView2 suruklenen dosyanin
        // gercek yolunu vermedigi icin icerik PARCALAR halinde base64 ile
        // akitiliyor -- bkz. Base64Decode.
        // -------------------------------------------------------------
        else if (cmd == "nvngxDropBegin")
        {
            std::wstring name = FromUtf8(msg.value("name", std::string()));
            for (auto& ch : name) ch = towlower(ch);

            if (g_nvngxUploadFile != INVALID_HANDLE_VALUE)
            {
                CloseHandle(g_nvngxUploadFile);
                g_nvngxUploadFile = INVALID_HANDLE_VALUE;
            }

            if (name != L"nvngx_dlssnr.dll")
            {
                json out; out["type"] = "nvngxDropResult";
                json d; d["ok"] = false; d["error"] = "Sadece nvngx_dlssnr.dll kabul edilir.";
                out["data"] = d;
                if (g_mainWebView) g_mainWebView->PostJson(FromUtf8(out.dump()));
                return;
            }

            g_nvngxUploadTempPath     = GetNvngxDlssnrPath() + L".part";
            g_nvngxUploadFile         = CreateFileW(g_nvngxUploadTempPath.c_str(), GENERIC_WRITE, 0, nullptr,
                                                     CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            g_nvngxUploadExpectedSize = msg.value("size", static_cast<uint64_t>(0));
            g_nvngxUploadReceivedSize = 0;
            g_nvngxUploadNextSeq      = 0;

            if (g_nvngxUploadFile == INVALID_HANDLE_VALUE)
            {
                json out; out["type"] = "nvngxDropResult";
                json d; d["ok"] = false; d["error"] = "Geçici dosya oluşturulamadı.";
                out["data"] = d;
                if (g_mainWebView) g_mainWebView->PostJson(FromUtf8(out.dump()));
            }
        }
        else if (cmd == "nvngxDropChunk")
        {
            if (g_nvngxUploadFile == INVALID_HANDLE_VALUE) return;

            int seq = msg.value("seq", -1);
            if (seq != g_nvngxUploadNextSeq)
            {
                CloseHandle(g_nvngxUploadFile);
                g_nvngxUploadFile = INVALID_HANDLE_VALUE;
                DeleteFileW(g_nvngxUploadTempPath.c_str());

                json out; out["type"] = "nvngxDropResult";
                json d; d["ok"] = false; d["error"] = "Aktarım sırası bozuldu, tekrar deneyin.";
                out["data"] = d;
                if (g_mainWebView) g_mainWebView->PostJson(FromUtf8(out.dump()));
                return;
            }

            auto bytes = Base64Decode(msg.value("dataB64", std::string()));
            DWORD written = 0;
            WriteFile(g_nvngxUploadFile, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
            g_nvngxUploadReceivedSize += written;
            g_nvngxUploadNextSeq++;
        }
        else if (cmd == "nvngxDropEnd")
        {
            bool ok = false;
            std::wstring error;

            if (g_nvngxUploadFile != INVALID_HANDLE_VALUE)
            {
                CloseHandle(g_nvngxUploadFile);
                g_nvngxUploadFile = INVALID_HANDLE_VALUE;

                if (g_nvngxUploadExpectedSize != 0 && g_nvngxUploadReceivedSize != g_nvngxUploadExpectedSize)
                {
                    error = L"Dosya boyutu uyuşmadı, aktarım bozuk olabilir.";
                    DeleteFileW(g_nvngxUploadTempPath.c_str());
                }
                else
                {
                    std::wstring finalPath = GetNvngxDlssnrPath();
                    DeleteFileW(finalPath.c_str()); // eskisi varsa uzerine yazabilmek icin
                    if (MoveFileW(g_nvngxUploadTempPath.c_str(), finalPath.c_str()))
                    {
                        ok = true;
                        DLSS_Log("[Main] nvngx_dlssnr.dll surukle-birak ile eklendi (%llu bayt).",
                                 static_cast<unsigned long long>(g_nvngxUploadReceivedSize));
                    }
                    else
                    {
                        error = L"Dosya taşınamadı (hata=" + std::to_wstring(GetLastError()) + L").";
                    }
                }
            }
            else
            {
                error = L"Aktarım durumu bulunamadı.";
            }

            json out; out["type"] = "nvngxDropResult";
            json d; d["ok"] = ok;
            if (!ok) d["error"] = ToUtf8(error);
            out["data"] = d;
            if (g_mainWebView) g_mainWebView->PostJson(FromUtf8(out.dump()));

            if (ok) PushStateToJs(); // BASLAT butonu/durum cubugu aninda guncellensin
        }
    }
    catch (...) { /* Hatali/beklenmedik JSON alani -- bu mesaji yoksay, uygulamayi cokertme. */ }
}

// ---------------------------------------------------------------------------
// WndProc
// ---------------------------------------------------------------------------
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        g_mainHwnd = hwnd;

        g_fullscreenStretch = ConfigManager::Get().Config().fullscreenStretch;

        // Faz 2: istemci alanini tamamen kaplayan gomulu WebView2 kontrolu.
        // WebViewHost::EnsureEnvironment zaten WinMain'de SettingsWindow::Initialize
        // (vb.) tarafindan bu pencere yaratilmadan ONCE cagrildi -- ortam hazir.
        //
        // KRITIK: CreateCoreWebView2Controller BURADA (WM_CREATE icinde) COK ERKEN --
        // pencere WinMain henuz ShowWindow(SW_SHOW) cagirmadigi icin HALA GORUNMEZ.
        // Bu durumda WebView2'nin dahili DirectComposition gorsel agaci pencereye
        // hicbir zaman duzgun baglanmiyor; kontrolor/navigasyon "basarili" rapor
        // etse (ve put_DefaultBackgroundColor bile "basarili" dese) BILE ekranda
        // SONSUZA KADAR bembeyaz kaliyor -- 4 popup pencere bunu YASAMIYOR cunku
        // onlar WS_POPUP | WS_VISIBLE ile, yani zaten GORUNUR halde olusturuluyor.
        // Cozum: gercek CreateEmbedded cagrisini, pencere WinMain'de ShowWindow ile
        // gosterildikten SONRAYA ertelemek -- kendimize postaladigimiz WM_APP+3
        // mesaji, ShowWindow/UpdateWindow bittikten SONRA, ana mesaj dongusu
        // basladiginda islenecek.
        g_mainWebView = std::make_unique<WebViewHost>();
        PostMessageW(hwnd, WM_APP + 3, 0, 0);

        // Global hotkey registration & fallback timer
        RegisterAppHotkey(hwnd);
        SetTimer(hwnd, IDT_HOTKEY_TIMER, 50, nullptr);

        // Populate initial lists (veri hazirla -- JS'e push WM_APP+3'te olacak)
        PopulateList(hwnd);
        PopulateGpuList(hwnd);
        break;
    }

    case WM_APP + 3:
    {
        // Bkz. WM_CREATE'teki aciklama: pencere artik WinMain'de ShowWindow ile
        // gosterilmis olmali (mesaj kuyruga ShowWindow'dan SONRA girdi) -- WebView2
        // kontrolunu simdi guvenle olusturabiliriz.
        if (g_mainWebView)
        {
            g_mainWebView->CreateEmbedded(hwnd, L"vlss5.main",
                ExeDirWebFolder(L"main").c_str(), L"index.html", &OnMainWebMessage);
            PushStateToJs();
        }
        break;
    }

    case WM_SIZE:
    {
        if (g_mainWebView)
        {
            RECT rc;
            GetClientRect(hwnd, &rc);
            g_mainWebView->Resize(rc);
        }
        break;
    }

    case WM_APP + 1:
    {
        // HotkeysWindow'dan rebind bildirimi -- hotkey'i yeniden kaydet ve
        // guncel etiketleri (RefreshStartButton == PushStateToJs) JS'e it.
        RegisterAppHotkey(hwnd);
        RefreshStartButton();
        break;
    }

    case WM_APP + 4:
    {
        // Bkz. OnMainWebMessage'daki openSettings/openRtss/openHotkeys/openPresets
        // aciklamasi: alt pencereleri burada, ana webview'in WebMessageReceived
        // callback'inin TAMAMEN disinda acariz -- boylece CreatePopup'in kendi
        // ic-ice WebView2 controller olusturma cagrisi asla reentrant bir COM
        // cagrisi icinde takilip 10sn timeout'a girip siyah kalmiyor.
        switch (static_cast<int>(wParam))
        {
        case 0: SettingsWindow::Toggle(hwnd); break;
        case 1: RtssWindow::Show(hwnd);       break;
        case 2: HotkeysWindow::Show(hwnd);    break;
        case 3: PresetsWindow::Show(hwnd);    break;
        }
        break;
    }

    case WM_APP_UPDATES_FETCH_DONE:
    {
        // Bkz. StartUpdatesCheck: FetchLatestReleases arka plan thread'inde
        // calisti, sonuc heap'te bize devredildi -- sahiplik burada, tek
        // cikis yolunda delete edilir.
        std::unique_ptr<UpdateChecker::FetchResult> result(
            reinterpret_cast<UpdateChecker::FetchResult*>(lParam));
        bool silentStartupCheck = (wParam == 1);

        g_updatesChecking = false;
        if (result->ok)
        {
            g_updatesCache = result->releases;
            g_updatesLastError.clear();
            g_updatesLatestTag = g_updatesCache.empty() ? L"" : g_updatesCache.front().tag;
            g_updatesHasNewer = !g_updatesLatestTag.empty() &&
                UpdateChecker::CompareVersions(g_updatesLatestTag, VLSS5_VERSION_STRING) > 0;
        }
        else
        {
            g_updatesLastError = result->error;
            DLSS_Log("[Updates] Surum kontrolu basarisiz: %ls", result->error.c_str());
        }
        PushUpdatesStateToJs();

        if (silentStartupCheck && g_updatesHasNewer)
        {
            std::wstring mainInstruction = L"Yeni güncelleme mevcut: " + g_updatesLatestTag + L". İndirilip kurulsun mu?";
            std::wstring content = L"Mevcut sürümünüz: " + std::wstring(VLSS5_VERSION_STRING);

            if (ConfirmDialog::AskYesNo(hwnd, L"VLSS5 - Güncelleme Mevcut",
                    mainInstruction.c_str(), content.c_str(),
                    L"Evet", L"Hayır", /*defaultIsYes*/true, /*warningIcon*/false))
            {
                if (!g_updatesCache.empty())
                {
                    if (const auto* asset = UpdateChecker::PickBestAsset(g_updatesCache.front()))
                        StartUpdatesDownload(hwnd, asset->downloadUrl, asset->name);
                }
            }
        }
        break;
    }

    case WM_APP_UPDATES_DL_PROGRESS:
    {
        std::unique_ptr<UpdateChecker::DownloadProgress> prog(
            reinterpret_cast<UpdateChecker::DownloadProgress*>(lParam));
        if (g_mainWebView)
        {
            json data;
            data["received"] = prog->received;
            data["total"]    = prog->total;
            json msg;
            msg["type"] = "updatesDownloadProgress";
            msg["data"] = data;
            g_mainWebView->PostJson(FromUtf8(msg.dump()));
        }
        break;
    }

    case WM_APP_UPDATES_DL_DONE:
    {
        std::unique_ptr<std::wstring> pathOrError(reinterpret_cast<std::wstring*>(lParam));
        bool ok = (wParam == 1);
        g_updatesDownloading = false;

        json data;
        data["ok"] = ok;
        if (ok)
        {
            data["path"] = ToUtf8(*pathOrError);

            std::wstring fileName = *pathOrError;
            size_t slash = fileName.find_last_of(L"\\/");
            if (slash != std::wstring::npos) fileName = fileName.substr(slash + 1);

            if (UpdateChecker::IsAutoInstallableAsset(fileName))
            {
                // Yeni surumler (bkz. installer/VLSS5.iss + .github/workflows/release.yml):
                // Setup.exe -- sessizce kur, VLSS5'i kapat, installer kurulum
                // bitince otomatik yeniden baslatir. Elle mudahale YOK.
                data["autoInstalling"] = true;
                LaunchSilentInstallAndExit(hwnd, *pathOrError);
            }
            else
            {
                // Eski surumlerin .rar asset'i (henuz yeni Setup.exe pipeline'iyla
                // yayimlanmamis) -- otomatik kuramayiz, kullanicinin varsayilan
                // arsiv programina devrediyoruz (bkz. presetsOpenFolder ile ayni desen).
                ShellExecuteW(hwnd, L"open", pathOrError->c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
        }
        else
        {
            data["error"] = ToUtf8(*pathOrError);
            DLSS_Log("[Updates] Indirme basarisiz: %ls", pathOrError->c_str());
        }

        if (g_mainWebView)
        {
            json msg;
            msg["type"] = "updatesDownloadDone";
            msg["data"] = data;
            g_mainWebView->PostJson(FromUtf8(msg.dump()));
        }
        PushUpdatesStateToJs();
        break;
    }

    case WM_HOTKEY:
        if (wParam == ID_GLOBAL_HOTKEY)
        {
            TriggerCaptureToggle(hwnd);
            return 0;
        }
        break;

    case WM_TIMER:
        if (wParam == IDT_HOTKEY_TIMER)
        {
            if (!InputForwarder::IsCapturing())
            {
                if (InputForwarder::IsStopKeyDown())
                {
                    TriggerCaptureToggle(hwnd);
                }
            }
            return 0;
        }
        break;

    case WM_DESTROY:
        KillTimer(hwnd, IDT_HOTKEY_TIMER);
        UnregisterAppHotkey(hwnd);
        InputForwarder::EndCapture();

        // WebView2 controller/webview COM nesnelerini ana pencere yok edilmeden
        // once temiz birak. WebViewHost::CreateEmbedded m_embedded=true kurdugu
        // icin destructor bu asamada DestroyWindow(hwnd) -- COKTAN yikilmakta
        // olan bu ayni pencereyi -- CAGIRMAZ -- bkz. WebViewHost.cpp.
        g_mainWebView.reset();

        PostQuitMessage(0);
        break;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// Ensure nvofapi64.dll is available (auto-copy from System32 if missing)
// ---------------------------------------------------------------------------
static void EnsureNvofapiAvailable()
{
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    PathRemoveFileSpecW(exePath);

    wchar_t targetNvof[MAX_PATH] = {};
    PathCombineW(targetNvof, exePath, L"nvofapi64.dll");

    wchar_t sysDir[MAX_PATH] = {};
    GetSystemDirectoryW(sysDir, MAX_PATH);
    wchar_t srcNvof[MAX_PATH] = {};
    PathCombineW(srcNvof, sysDir, L"nvofapi64.dll");

    // If nvofapi64.dll is missing in the .exe directory, automatically copy from System32
    if (GetFileAttributesW(targetNvof) == INVALID_FILE_ATTRIBUTES)
    {
        if (GetFileAttributesW(srcNvof) != INVALID_FILE_ATTRIBUTES)
        {
            if (CopyFileW(srcNvof, targetNvof, FALSE))
            {
                DLSS_Log("[Init] nvofapi64.dll System32'den basariyla uygulama klasorune kopyalandi.");
            }
            else
            {
                DLSS_Log("[Init] nvofapi64.dll kopyalanamadi (hata=%lu), dogrudan System32'den yuklenecek.", GetLastError());
            }
        }
        else
        {
            DLSS_Log("[Init] Bilgi: System32 altinda nvofapi64.dll bulunamadi.");
        }
    }

    // Preload nvofapi64.dll into process memory
    HMODULE hNvof = LoadLibraryW(targetNvof);
    if (!hNvof && GetFileAttributesW(srcNvof) != INVALID_FILE_ATTRIBUTES)
    {
        hNvof = LoadLibraryW(srcNvof);
    }
    if (hNvof)
    {
        DLSS_Log("[Init] nvofapi64.dll basariyla bellege yuklendi (0x%p).", hNvof);
    }
}

// ---------------------------------------------------------------------------
// CheckRequiredFiles
// ---------------------------------------------------------------------------
void CheckRequiredFiles()
{
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    PathRemoveFileSpecW(exePath);

    wchar_t file1[MAX_PATH] = {};
    PathCombineW(file1, exePath, L"nvngx.dll_dlssnr.dll");

    if (GetFileAttributesW(file1) == INVALID_FILE_ATTRIBUTES)
    {
        MessageBoxW(nullptr,
            L"Önemli bir dll dosyası ana klasörde bulunamadı! nvngx.dll_dllsnr.dll dosyasını lütfen geri yükleyin. Bu dosya DLSS5 dosyası değildir, programa ait bir .dll'dir ve programın yanında olmak zorundadır.",
            L"VLSS5 Hata", MB_ICONERROR | MB_OK | MB_TOPMOST);
        ExitProcess(1);
    }

    // nvngx_dlssnr.dll (telifli NGX model agirliklari) ARTIK burada sert bir
    // hata/ExitProcess ile zorunlu tutulmuyor -- Ana Sayfa'daki durum/surukle-
    // birak cubugu (bkz. dllStatusBar, NvngxDlssnrExists) kullaniciya dosyayi
    // uygulama ICINDEN eklemesini saglar. Dosya yoksa sadece BASLAT devre disi
    // kalir (bkz. BuildStateJson "nvngxDlssnrReady" ve "startStop" isleyicisi).
}

// ---------------------------------------------------------------------------
// CheckRivaTuner
// ---------------------------------------------------------------------------
static void CheckRivaTuner()
{
    std::wstring rtssDir = ConfigManager::Get().Config().rtssDirectory;
    bool found = false;
    
    // Check configured directory first
    if (!rtssDir.empty() && GetFileAttributesW(rtssDir.c_str()) != INVALID_FILE_ATTRIBUTES)
    {
        found = true;
    }
    
    // Check default RTSS directory if not found
    if (!found)
    {
        std::wstring profilesDir = RTSSManager::Get().GetProfilesDir();
        if (GetFileAttributesW(profilesDir.c_str()) != INVALID_FILE_ATTRIBUTES)
        {
            found = true;
        }
    }

    if (!found)
    {
        typedef HRESULT (WINAPI *TaskDialogIndirect_t)(const TASKDIALOGCONFIG*, int*, int*, BOOL*);
        HMODULE hComCtl = LoadLibraryW(L"comctl32.dll");
        TaskDialogIndirect_t pTaskDialogIndirect = hComCtl ? (TaskDialogIndirect_t)GetProcAddress(hComCtl, "TaskDialogIndirect") : nullptr;

        if (pTaskDialogIndirect)
        {
            TASKDIALOGCONFIG tdc = { sizeof(TASKDIALOGCONFIG) };
            tdc.hwndParent = nullptr;
            tdc.hInstance = GetModuleHandle(nullptr);
            tdc.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
            tdc.pszWindowTitle = L"VLSS5 - RivaTuner Eksik";
            tdc.pszMainInstruction = L"DİKKAT! Bilgisayarınızda RivaTuner tespit edilemedi.";
            tdc.pszContent = L"Oyunlarda otomatik FPS kalibrasyonu çalışmayacaktır. Lütfen RivaTuner'ı bilgisayarınıza kurun!\n\n(Eğer RivaTuner yüklüyse ancak farklı bir dizindeyse, lütfen program açıldıktan sonra 'RTSS AYARLARI' menüsünden klasör yolunu manuel olarak seçin.)";
            tdc.pszMainIcon = TD_WARNING_ICON;

            TASKDIALOG_BUTTON buttons[] = {
                { 1001, L"İndir" },
                { 1002, L"Tamam" }
            };
            tdc.cButtons = 2;
            tdc.pButtons = buttons;
            tdc.nDefaultButton = 1001;

            int selectedButton = 0;
            HRESULT hr = pTaskDialogIndirect(&tdc, &selectedButton, nullptr, nullptr);

            if (SUCCEEDED(hr) && selectedButton == 1001)
            {
                ShellExecuteW(nullptr, L"open", L"https://www.msi.com/Landing/afterburner", nullptr, nullptr, SW_SHOWNORMAL);
            }
        }
        else
        {
            int res = MessageBoxW(nullptr, 
                L"DİKKAT! Bilgisayarınızda RivaTuner tespit edilemedi.\n\nOyunlarda otomatik FPS kalibrasyonu çalışmayacaktır. Lütfen RivaTuner'ı bilgisayarınıza kurun!\n\n(Eğer RivaTuner yüklüyse ancak farklı bir dizindeyse, lütfen program açıldıktan sonra 'RTSS AYARLARI' menüsünden klasör yolunu manuel olarak seçin.)\n\nİndirme sayfasına gitmek için 'Evet'e basın.", 
                L"VLSS5 - RivaTuner Eksik", 
                MB_ICONWARNING | MB_YESNO | MB_TOPMOST);
            
            if (res == IDYES)
            {
                ShellExecuteW(nullptr, L"open", L"https://www.msi.com/Landing/afterburner", nullptr, nullptr, SW_SHOWNORMAL);
            }
        }
    }
}

static bool IsProcessRunning(const wchar_t* processName)
{
    bool exists = false;
    PROCESSENTRY32W entry;
    entry.dwSize = sizeof(PROCESSENTRY32W);

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, NULL);
    if (snapshot == INVALID_HANDLE_VALUE)
        return false;

    if (Process32FirstW(snapshot, &entry))
    {
        do
        {
            if (_wcsicmp(entry.szExeFile, processName) == 0)
            {
                exists = true;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return exists;
}

static void CheckRivaTunerRunning()
{
    if (ConfigManager::Get().Config().suppressRtssRunningWarning)
        return;

    if (IsProcessRunning(L"RTSS.exe"))
        return;

    const wchar_t* msg =
        L"UYARI! Arka planda RivaTuner çalışmadığı tespit edildi. Lütfen FPS kalibrasyonu için arkada uygulamayı açık bırakın. Kalibrasyon yapılmadığı sürece FPS çok kötü olabilir.";

    typedef HRESULT (WINAPI *TaskDialogIndirect_t)(const TASKDIALOGCONFIG*, int*, int*, BOOL*);
    HMODULE hComCtl = LoadLibraryW(L"comctl32.dll");
    TaskDialogIndirect_t pTaskDialogIndirect = hComCtl ? (TaskDialogIndirect_t)GetProcAddress(hComCtl, "TaskDialogIndirect") : nullptr;

    if (pTaskDialogIndirect)
    {
        TASKDIALOGCONFIG tdc = { sizeof(TASKDIALOGCONFIG) };
        tdc.hwndParent            = nullptr;
        tdc.hInstance             = GetModuleHandle(nullptr);
        tdc.dwFlags               = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
        tdc.pszWindowTitle        = L"VLSS5 - RivaTuner Çalışmıyor";
        tdc.pszMainInstruction    = L"UYARI! Arka planda RivaTuner çalışmadığı tespit edildi.";
        tdc.pszContent            = L"Lütfen FPS kalibrasyonu için arkada uygulamayı açık bırakın. Kalibrasyon yapılmadığı sürece FPS çok kötü olabilir.";
        tdc.pszMainIcon           = TD_WARNING_ICON;
        tdc.pszVerificationText   = L"Bir daha gösterme";
        tdc.dwCommonButtons       = TDCBF_OK_BUTTON;

        BOOL verificationChecked = FALSE;
        HRESULT hr = pTaskDialogIndirect(&tdc, nullptr, nullptr, &verificationChecked);

        if (SUCCEEDED(hr) && verificationChecked)
        {
            ConfigManager::Get().Config().suppressRtssRunningWarning = true;
            ConfigManager::Get().Save();
        }
    }
    else
    {
        MessageBoxW(nullptr, msg, L"VLSS5 - RivaTuner Çalışmıyor", MB_ICONWARNING | MB_OK | MB_TOPMOST);
    }
}

// ---------------------------------------------------------------------------
// WinMain
// ---------------------------------------------------------------------------
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow)
{
    CrashHandler::Install();

    CheckRequiredFiles();
    CheckRivaTuner();
    CheckRivaTunerRunning();

    // Single instance check: prevent multiple instances of VLSS5 from running simultaneously
    HANDLE hSingleInstanceMutex = CreateMutexW(nullptr, TRUE, L"Local\\VLSS5_SingleInstance_Mutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS || !hSingleInstanceMutex)
    {
        if (hSingleInstanceMutex)
        {
            CloseHandle(hSingleInstanceMutex);
        }

        // Bring existing window to front if it's visible
        HWND existingHwnd = FindWindowW(L"VLSS5Main", nullptr);
        if (existingHwnd && IsWindow(existingHwnd))
        {
            if (IsIconic(existingHwnd))
            {
                ShowWindow(existingHwnd, SW_RESTORE);
            }
            SetForegroundWindow(existingHwnd);
        }

        MessageBoxW(
            nullptr,
            L"VLSS5 zaten açık, lütfen önce VLSS5'i kapatın ve yeniden deneyin.",
            L"VLSS5",
            MB_ICONWARNING | MB_OK | MB_TOPMOST);
        return 0;
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // Hedef listesindeki pencere simgelerini (HICON) PNG'ye kodlamak icin --
    // bkz. IconToDataUri. PopulateList'ten (dolayisiyla herhangi bir pencere
    // olusturulmadan) ONCE baslatilmali.
    ULONG_PTR gdiplusToken = 0;
    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, nullptr);

    winrt::init_apartment(winrt::apartment_type::single_threaded);

    // Disable OS background frame-rate throttling
    {
        PROCESS_POWER_THROTTLING_STATE ppt = {};
        ppt.Version     = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        ppt.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
        ppt.StateMask   = 0;
        SetProcessInformation(GetCurrentProcess(),
            ProcessPowerThrottling, &ppt, sizeof(ppt));
    }
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);

    // Automatically ensure nvofapi64.dll is present and preloaded
    EnsureNvofapiAvailable();

    // Automatically ensure _nvngx.dll and nvngx.dll are present and up to date
    EnsureNGXAvailable();

    DLSS_Log("[Main] VLSS5 launcher started.");

    // Register main window class with custom V5 icon
    HICON hMainIcon   = reinterpret_cast<HICON>(LoadImageW(
        hInstance, MAKEINTRESOURCEW(IDI_MAIN_ICON), IMAGE_ICON,
        GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
    HICON hMainIconSm = reinterpret_cast<HICON>(LoadImageW(
        hInstance, MAKEINTRESOURCEW(IDI_MAIN_ICON), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    if (!hMainIcon)   hMainIcon   = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_MAIN_ICON));
    if (!hMainIconSm) hMainIconSm = hMainIcon;

    WNDCLASSEXW wc   = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"VLSS5Main";
    wc.hIcon         = hMainIcon;
    wc.hIconSm       = hMainIconSm;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    // Initialize DLSS 5 settings window class and common controls
    SettingsWindow::Initialize(hInstance);
    RtssWindow::Initialize(hInstance);
    HotkeysWindow::Initialize(hInstance);
    PresetsWindow::Initialize(hInstance);

    // Faz 3: "Tuşları Değiştir" sekmesi HotkeysWindow'un KENDI WH_KEYBOARD_LL
    // hook'unu kullanir (bkz. OnMainWebMessage'daki hotkeysStartCapture) ama
    // ana pencere sekmesi HotkeysWindow'un s_host'undan FARKLI bir WebViewHost
    // (g_mainWebView) kullandigi icin sonucu bu callback ile ayrica alir.
    HotkeysWindow::SetExternalKeyCapturedCallback([](bool /*save*/, UINT /*vk*/, UINT /*mod*/)
    {
        if (!g_mainWebView) return;

        json ack;
        ack["type"] = "hotkeysKeyCaptured";
        g_mainWebView->PostJson(FromUtf8(ack.dump()));

        json labelsMsg;
        labelsMsg["type"] = "hotkeysList";
        labelsMsg["data"] = BuildHotkeyLabelsJson();
        g_mainWebView->PostJson(FromUtf8(labelsMsg.dump()));
    });

    // Create modern dark window (fixed size, centered, styled)
    const int kMainWndW = 1280, kMainWndH = 720;
    const int mainWndX = (GetSystemMetrics(SM_CXSCREEN) - kMainWndW) / 2;
    const int mainWndY = (GetSystemMetrics(SM_CYSCREEN) - kMainWndH) / 2;
    HWND hwnd = CreateWindowExW(
        0,
        L"VLSS5Main",
        L"VLSS5",
        (WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX)),
        mainWndX, mainWndY,
        kMainWndW, kMainWndH,
        nullptr, nullptr, hInstance, nullptr);

    if (!hwnd)
    {
        MessageBoxW(nullptr, L"Pencere oluşturulamadı.", L"VLSS5", MB_ICONERROR);
        return -1;
    }

    // Windows 11 / Modern Dark Title Bar & Rounded Corners
    BOOL darkMode = TRUE;
    DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &darkMode, sizeof(darkMode));
    DWORD cornerPref = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(hwnd, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &cornerPref, sizeof(cornerPref));

    if (hMainIcon)
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(hMainIcon));
    if (hMainIconSm)
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(hMainIconSm));

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    // Create App instance
    g_app = std::make_unique<App>(hInstance);
    SettingsWindow::SetAppInstance(g_app.get());

    // "Güncellemeler" sekmesi: kullanici acmasa bile her baslangicta sessizce
    // GitHub Releases kontrol edilir (ayar kapatilabilir, bkz. ConfigManager
    // autoCheckUpdates / web/main "Güncellemeler" sekmesindeki anahtar).
    // Yeni surum bulunursa WM_APP_UPDATES_FETCH_DONE isleyicisi onay dialogu gosterir.
    if (ConfigManager::Get().Config().autoCheckUpdates)
    {
        StartUpdatesCheck(hwnd, /*silentStartupCheck*/true);
    }

    // Standard Win32 message loop
    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_app.reset();
    winrt::uninit_apartment();
    Gdiplus::GdiplusShutdown(gdiplusToken);

    if (hSingleInstanceMutex)
    {
        ReleaseMutex(hSingleInstanceMutex);
        CloseHandle(hSingleInstanceMutex);
    }

    return static_cast<int>(msg.wParam);
}
