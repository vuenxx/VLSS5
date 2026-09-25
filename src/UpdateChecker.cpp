#include "UpdateChecker.h"
#include "../third_party/json/json.hpp"
#include <winhttp.h>
#include <vector>

#pragma comment(lib, "winhttp.lib")

using json = nlohmann::json;

namespace
{
    // Diger dosyalardaki (Main.cpp, SettingsWindow.cpp vb.) ayni AYRI-kopya
    // desen: kucuk UTF-8 <-> wstring yardimcilari ortak bir header'a cikarilmaz.
    std::string ToUtf8(const std::wstring& w)
    {
        if (w.empty()) return {};
        int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string out(len > 0 ? len - 1 : 0, '\0');
        if (len > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), len, nullptr, nullptr);
        return out;
    }
    std::wstring FromUtf8(const std::string& s)
    {
        if (s.empty()) return {};
        int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
        std::wstring out(len > 0 ? len - 1 : 0, L'\0');
        if (len > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), len);
        return out;
    }

    // RAII: WinHTTP handle'lari erken 'return' yollarinda bile kapatilsin.
    struct HInternetGuard
    {
        HINTERNET h = nullptr;
        ~HInternetGuard() { if (h) WinHttpCloseHandle(h); }
    };

    std::wstring LastErrorMessage(const wchar_t* what)
    {
        wchar_t buf[256];
        swprintf_s(buf, L"%s (WinHTTP hata=%lu)", what, GetLastError());
        return buf;
    }

    // hRequest uzerinden yaniti sonuna kadar okuyup ham byte olarak dondurur.
    bool ReadAllBody(HINTERNET hRequest, std::string& outBody, std::wstring& error)
    {
        outBody.clear();
        for (;;)
        {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(hRequest, &available))
            {
                error = LastErrorMessage(L"WinHttpQueryDataAvailable basarisiz");
                return false;
            }
            if (available == 0) break;

            std::vector<char> chunk(available);
            DWORD read = 0;
            if (!WinHttpReadData(hRequest, chunk.data(), available, &read))
            {
                error = LastErrorMessage(L"WinHttpReadData basarisiz");
                return false;
            }
            outBody.append(chunk.data(), read);
        }
        return true;
    }

    DWORD QueryStatusCode(HINTERNET hRequest)
    {
        DWORD statusCode = 0;
        DWORD size = sizeof(statusCode);
        WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &size, WINHTTP_NO_HEADER_INDEX);
        return statusCode;
    }
}

namespace UpdateChecker
{

FetchResult FetchLatestReleases(int count)
{
    FetchResult result;
    if (count < 1) count = 1;
    if (count > 30) count = 30; // GitHub API tek sayfa ust siniri

    HInternetGuard session{ WinHttpOpen(L"VLSS5-UpdateChecker/1.0",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0) };
    if (!session.h)
    {
        result.error = LastErrorMessage(L"WinHttpOpen basarisiz");
        return result;
    }
    WinHttpSetTimeouts(session.h, 8000, 8000, 8000, 15000);

    HInternetGuard connect{ WinHttpConnect(session.h, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0) };
    if (!connect.h)
    {
        result.error = LastErrorMessage(L"WinHttpConnect basarisiz");
        return result;
    }

    wchar_t path[256];
    swprintf_s(path, L"/repos/vuenxx/VLSS5/releases?per_page=%d", count);

    HInternetGuard request{ WinHttpOpenRequest(connect.h, L"GET", path, nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) };
    if (!request.h)
    {
        result.error = LastErrorMessage(L"WinHttpOpenRequest basarisiz");
        return result;
    }

    // html+json: yanitta ham markdown 'body' yaninda GitHub'in ONCEDEN
    // RENDER ETTIGI 'body_html' alanini da getirir -- kendi markdown
    // parser'imizi yazmamiza gerek kalmaz.
    const wchar_t* headers = L"Accept: application/vnd.github.html+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
    WinHttpAddRequestHeaders(request.h, headers, (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD);

    if (!WinHttpSendRequest(request.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
    {
        result.error = LastErrorMessage(L"WinHttpSendRequest basarisiz");
        return result;
    }
    if (!WinHttpReceiveResponse(request.h, nullptr))
    {
        result.error = LastErrorMessage(L"WinHttpReceiveResponse basarisiz");
        return result;
    }

    DWORD status = QueryStatusCode(request.h);
    if (status != 200)
    {
        wchar_t buf[128];
        swprintf_s(buf, L"GitHub API HTTP %lu döndürdü", status);
        result.error = buf;
        return result;
    }

    std::string body;
    if (!ReadAllBody(request.h, body, result.error))
        return result;

    try
    {
        json arr = json::parse(body);
        if (!arr.is_array())
        {
            result.error = L"Beklenmeyen API yaniti (dizi degil)";
            return result;
        }

        for (auto& r : arr)
        {
            // Taslak (draft) surumler henuz yayimlanmamistir -- listelenmez.
            if (r.value("draft", false)) continue;

            ReleaseInfo info;
            info.tag         = FromUtf8(r.value("tag_name", std::string()));
            info.name        = FromUtf8(r.value("name", std::string()));
            if (info.name.empty()) info.name = info.tag;
            info.bodyHtml    = FromUtf8(r.value("body_html", std::string()));
            info.publishedAt = FromUtf8(r.value("published_at", std::string()));
            info.htmlUrl     = FromUtf8(r.value("html_url", std::string()));
            info.prerelease  = r.value("prerelease", false);

            if (r.contains("assets") && r["assets"].is_array())
            {
                for (auto& a : r["assets"])
                {
                    ReleaseAsset asset;
                    asset.name        = FromUtf8(a.value("name", std::string()));
                    asset.downloadUrl = FromUtf8(a.value("browser_download_url", std::string()));
                    asset.size        = a.value("size", (uint64_t)0);
                    info.assets.push_back(std::move(asset));
                }
            }

            result.releases.push_back(std::move(info));
            if ((int)result.releases.size() >= count) break;
        }

        result.ok = true;
    }
    catch (const std::exception& e)
    {
        result.error = L"JSON ayrıştırma hatası: " + FromUtf8(e.what());
    }

    return result;
}

bool IsAutoInstallableAsset(const std::wstring& fileName)
{
    // "VLSS5-Setup-0.6.0.exe" gibi -- installer/VLSS5.iss'teki
    // OutputBaseFilename ile ayni desen. Tam eslesme yerine kaba bir icerik
    // kontrolu yapiyoruz ki .iss'teki isimlendirme kucuk bir sekilde
    // degisirse (orn. surum eki) kirilmasin.
    std::wstring lower = fileName;
    for (auto& ch : lower) ch = towlower(ch);

    bool endsWithExe = lower.size() >= 4 && lower.compare(lower.size() - 4, 4, L".exe") == 0;
    bool hasSetup    = lower.find(L"setup") != std::wstring::npos;
    return endsWithExe && hasSetup;
}

const ReleaseAsset* PickBestAsset(const ReleaseInfo& release)
{
    for (auto& a : release.assets)
    {
        if (IsAutoInstallableAsset(a.name)) return &a;
    }
    return release.assets.empty() ? nullptr : &release.assets.front();
}

int CompareVersions(const std::wstring& a, const std::wstring& b)
{
    auto parse = [](const std::wstring& v) -> std::vector<int>
    {
        std::wstring s = v;
        if (!s.empty() && (s[0] == L'v' || s[0] == L'V')) s = s.substr(1);

        std::vector<int> parts;
        size_t i = 0;
        while (i < s.size())
        {
            size_t start = i;
            while (i < s.size() && iswdigit(s[i])) ++i;
            if (i > start)
                parts.push_back(_wtoi(s.substr(start, i - start).c_str()));
            else
                ++i; // sayisal olmayan ayirici (., -, +) atla
            if (i < s.size() && s[i] != L'.' && !iswdigit(s[i]))
            {
                // "0.5.1-beta" gibi ekler -- sayisal kisimdan sonrasini yoksay.
                break;
            }
        }
        return parts;
    };

    std::vector<int> pa = parse(a), pb = parse(b);
    size_t n = (pa.size() > pb.size()) ? pa.size() : pb.size();
    for (size_t i = 0; i < n; ++i)
    {
        int va = (i < pa.size()) ? pa[i] : 0;
        int vb = (i < pb.size()) ? pb[i] : 0;
        if (va != vb) return (va < vb) ? -1 : 1;
    }
    return 0;
}

bool DownloadFile(const std::wstring& url, const std::wstring& outPath,
                   const ProgressCallback& onProgress, std::wstring& error)
{
    URL_COMPONENTS uc = { sizeof(uc) };
    wchar_t hostBuf[256] = {};
    wchar_t pathBuf[2048] = {};
    uc.lpszHostName    = hostBuf;
    uc.dwHostNameLength = _countof(hostBuf);
    uc.lpszUrlPath     = pathBuf;
    uc.dwUrlPathLength = _countof(pathBuf);

    if (!WinHttpCrackUrl(url.c_str(), (DWORD)url.size(), 0, &uc))
    {
        error = LastErrorMessage(L"Geçersiz indirme adresi");
        return false;
    }

    HInternetGuard session{ WinHttpOpen(L"VLSS5-UpdateChecker/1.0",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0) };
    if (!session.h)
    {
        error = LastErrorMessage(L"WinHttpOpen basarisiz");
        return false;
    }
    WinHttpSetTimeouts(session.h, 8000, 8000, 15000, 60000);

    HInternetGuard connect{ WinHttpConnect(session.h, hostBuf, uc.nPort, 0) };
    if (!connect.h)
    {
        error = LastErrorMessage(L"WinHttpConnect basarisiz");
        return false;
    }

    DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HInternetGuard request{ WinHttpOpenRequest(connect.h, L"GET", pathBuf, nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags) };
    if (!request.h)
    {
        error = LastErrorMessage(L"WinHttpOpenRequest basarisiz");
        return false;
    }

    // GitHub asset indirme baglantilari objects.githubusercontent.com'a 302
    // ile yonlendirir -- WinHTTP bunu varsayilan olarak otomatik izler.
    if (!WinHttpSendRequest(request.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
    {
        error = LastErrorMessage(L"WinHttpSendRequest basarisiz");
        return false;
    }
    if (!WinHttpReceiveResponse(request.h, nullptr))
    {
        error = LastErrorMessage(L"WinHttpReceiveResponse basarisiz");
        return false;
    }

    DWORD status = QueryStatusCode(request.h);
    if (status != 200)
    {
        wchar_t buf[128];
        swprintf_s(buf, L"İndirme HTTP %lu döndürdü", status);
        error = buf;
        return false;
    }

    uint64_t total = 0;
    {
        wchar_t lenBuf[32] = {};
        DWORD lenSize = sizeof(lenBuf);
        if (WinHttpQueryHeaders(request.h, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX,
                                 lenBuf, &lenSize, WINHTTP_NO_HEADER_INDEX))
        {
            total = _wcstoui64(lenBuf, nullptr, 10);
        }
    }

    HANDLE hFile = CreateFileW(outPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        error = L"Dosya oluşturulamadı: " + outPath;
        return false;
    }

    uint64_t received = 0;
    bool ok = true;
    for (;;)
    {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.h, &available))
        {
            error = LastErrorMessage(L"WinHttpQueryDataAvailable basarisiz");
            ok = false;
            break;
        }
        if (available == 0) break;

        std::vector<char> chunk(available);
        DWORD read = 0;
        if (!WinHttpReadData(request.h, chunk.data(), available, &read))
        {
            error = LastErrorMessage(L"WinHttpReadData basarisiz");
            ok = false;
            break;
        }

        DWORD written = 0;
        if (!WriteFile(hFile, chunk.data(), read, &written, nullptr) || written != read)
        {
            error = L"Diske yazma hatası (disk dolu olabilir)";
            ok = false;
            break;
        }

        received += read;
        if (onProgress) onProgress(DownloadProgress{ received, total });
    }

    CloseHandle(hFile);
    if (!ok)
    {
        DeleteFileW(outPath.c_str());
        return false;
    }
    return true;
}

}
