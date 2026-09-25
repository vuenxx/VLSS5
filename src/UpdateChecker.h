#pragma once
#include "Common.h"
#include <functional>
#include <vector>
#include <cstdint>

// ---------------------------------------------------------------------------
// UpdateChecker
//   GitHub Releases API (vuenxx/VLSS5) uzerinden surum kontrolu ve indirme.
//   Butun fonksiyonlar SENKRON/BLOKLAYICI -- her zaman bir arka plan
//   (std::thread) uzerinden cagirin, asla UI thread'inden dogrudan degil.
// ---------------------------------------------------------------------------
namespace UpdateChecker
{
    struct ReleaseAsset
    {
        std::wstring name;
        std::wstring downloadUrl;
        uint64_t     size = 0;
    };

    struct ReleaseInfo
    {
        std::wstring tag;          // orn. "v0.5.1"
        std::wstring name;         // release basligi
        std::wstring bodyHtml;     // GitHub'in render ettigi HTML sürum notlari
        std::wstring publishedAt;  // ISO8601
        std::wstring htmlUrl;      // github.com/.../releases/tag/... sayfasi
        bool         prerelease = false;
        std::vector<ReleaseAsset> assets;
    };

    struct FetchResult
    {
        bool                     ok = false;
        std::wstring             error;
        std::vector<ReleaseInfo> releases; // en yeniden eskiye, en fazla 'count' adet
    };

    // GitHub Releases API'den en son 'count' surumu ceker (varsayilan 5).
    FetchResult FetchLatestReleases(int count = 5);

    // fileName (uzantisiyla) installer/VLSS5.iss'in urettigi "VLSS5-Setup-*.exe"
    // deseniyle eslesiyor mu? Eslesirse indirilen dosya sessizce/otomatik
    // kurulabilir (bkz. Main.cpp LaunchSilentInstallAndExit); eslesmezse
    // (orn. eski surumlerin .rar asset'i) sadece varsayilan programla acilir.
    bool IsAutoInstallableAsset(const std::wstring& fileName);

    // Bir release'in asset listesinden OTOMATIK KURULUMA en uygun olani secer:
    // IsAutoInstallableAsset ile eslesen ilk asset varsa o, yoksa ilk asset
    // (eski davranisla geriye donuk uyumluluk). Asset yoksa nullptr doner.
    const ReleaseAsset* PickBestAsset(const ReleaseInfo& release);

    // "v1.2.3" / "1.2.3" bicimlerini sayisal olarak karsilastirir.
    // a > b ise pozitif, a < b ise negatif, esitse 0 doner.
    int CompareVersions(const std::wstring& a, const std::wstring& b);

    struct DownloadProgress
    {
        uint64_t received = 0;
        uint64_t total    = 0; // bilinmiyorsa 0
    };
    using ProgressCallback = std::function<void(const DownloadProgress&)>;

    // url'deki dosyayi outPath'e indirir. Basarisizlikta false doner ve error doldurulur.
    bool DownloadFile(const std::wstring& url, const std::wstring& outPath,
                       const ProgressCallback& onProgress, std::wstring& error);
}
