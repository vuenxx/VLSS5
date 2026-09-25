#pragma once
#include "Common.h"
#include <thread>
#include <mutex>

// Yakalama backend'i. WGC = Windows.Graphics.Capture (varsayilan, her durumda calisir).
// DXGIDuplication = IDXGIOutputDuplication (deneysel; pencere modunda WGC'nin
// CreateForWindow yolundaki bulaniklastirmayi bypass eder, monitor bazli calisip hedef
// dikdortgene GPU-tarafi crop uygular). DuplicateOutput basarisiz olursa Start/StartMonitor
// otomatik WGC'ye duser.
enum class CaptureBackend { WGC, DXGIDuplication };

// ---------------------------------------------------------------------------
// CaptureManager
//   Windows Graphics Capture (WGC) veya DXGI Desktop Duplication ile bir HWND'yi
//   ya da tam bir monitoru D3D11 dokusuna yakalar.
// ---------------------------------------------------------------------------
class CaptureManager
{
public:
    CaptureManager()  = default;
    ~CaptureManager() { Stop(); }

    // Begin capturing the target HWND. 'backend' == DXGIDuplication yakalanamazsa
    // (ornegin adapter uyusmazligi) otomatik WGC'ye duser.
    // Returns false if neither backend can be started or the HWND is invalid.
    bool Start(HWND targetHwnd, ID3D11Device* device, CaptureBackend backend = CaptureBackend::WGC);

    // Begin capturing an entire monitor ("whole screen" mode) instead of one window.
    // Returns false if neither backend can be started or the HMONITOR is invalid.
    bool StartMonitor(HMONITOR targetMonitor, ID3D11Device* device, CaptureBackend backend = CaptureBackend::WGC);

    // DXGIDuplication + pencere modunda: bu kare icin ekran-koordinatli crop dikdortgenini
    // gunceller (App::UpdateOverlayPosition zaten her kare hedef dikdortgenini hesapliyor --
    // buraya oldugu gibi aktarilir). WGC ya da monitor modunda no-op.
    bool SetCropRect(const RECT& screenRect);

    CaptureBackend GetBackend() const { return m_backend; }

    // Bir HWND'nin GERCEK yakalanabilir icerik boyutunu (WGC'nin GraphicsCaptureItem::Size()
    // uzerinden) sorgular -- gecici bir WGC item olusturup kapatir, capture BASLATMAZ.
    // DXGI Desktop Duplication + pencere modunda crop BOYUTU icin kullanilir: bazi oyunlarda
    // (ozellikle eski motorlar) GetClientRect/DWM sinirlarinin raporladigi pencere istemci
    // boyutu, oyunun GERCEKTEN render ettigi ic bolgeyle uyusmuyor -- WGC'nin kendi capture
    // yolu (CreateForWindow calisiyorsa) bu farktan etkilenmiyor cunku dogrudan DWM composition
    // metadata'sindan okuyor; ayni gercek kaynagi burada da kullaniyoruz.
    // Basarisiz olursa false doner, out parametreler dokunulmadan kalir.
    static bool QueryWindowContentSize(HWND targetHwnd, int& outWidth, int& outHeight);

    // Stop capturing and release all WGC resources.
    void Stop();

    bool IsActive()              const { return m_active; }
    bool IsNewFrameAvailable()   const { return m_newFrame.load(std::memory_order_relaxed); }
    HANDLE GetFrameEvent()       const { return m_frameEvent; }

    // WGC oturumu kendi icinden koptuysa (GraphicsCaptureItem::Closed) ya da sicak yoldaki
    // (AcquireCurrentFrameSRV/CopyFrame/FPS probe) bir WinRT cagrisi istisna firlattiysa true
    // olur. m_active/IsNewFrameAvailable bu durumda YANLIS bir sekilde "her sey normal ama
    // henuz yeni kare gelmedi" gibi gorunmeye devam eder -- gercekte oturum bir daha HICBIR
    // kare uretmeyecektir. App::Run bunu her turda kontrol edip yakalamayi yeniden baslatmali;
    // aksi halde overlay sessizce donmus/bos kalir (bkz. proje notlari: Chrome'da "hicbir sey
    // uygulanmiyor, FPS bile gozukmuyor" raporu).
    bool IsSessionBroken()       const { return m_sessionBroken.load(std::memory_order_relaxed); }

    int  GetWidth()              const { return m_width;  }
    int  GetHeight()             const { return m_height; }
    double GetLastFrameGapMs()   const { return m_lastFrameGapMs; }
    int  GetCurrentInputFps()    const { return m_currentInputFps; }

    // Bayat-korumali giris FPS'i. m_currentInputFps YALNIZCA yeni bir WGC karesi
    // geldiginde guncellenir; oyun donarsa ya da pencere occluded olursa son deger
    // sonsuza kadar oldugu gibi kalir. Kalibrasyon bu bayat degeri gercek sanip
    // sonsuz "desenkron" dongusune giriyordu. Son kare maxAge'den eskiyse 0 doneriz.
    int  GetInputFpsFresh(double maxAgeSeconds = 1.0) const;

    // Kalibrasyon FPS limitini degistirdikten sonra olcum penceresini sifirlar;
    // boylece bir sonraki okuma eski rejimin karelerini icermez.
    void ResetInputFpsWindow();
    const double* GetGapHistory()const { return m_gapHistory; }
    int  GetGapHistoryIdx()      const { return m_gapHistoryIdx; }

    // Zero-Copy Pipeline:
    // Acquires the freshest frame's SRV directly without intermediate CopyResource.
    // The underlying frame surface remains locked until ReleaseCurrentFrame() is called.
    // Returns nullptr if no new frame is ready or if resize occurred.
    ID3D11ShaderResourceView* AcquireCurrentFrameSRV(ID3D11Device* device);

    // Call immediately after rendering/presenting to return the frame surface to WGC pool.
    void ReleaseCurrentFrame();

    // Legacy copy fallback (copies latest captured frame into 'dst').
    bool CopyFrame(ID3D11DeviceContext* ctx, ID3D11Texture2D* dst);

    // Returns true (and resets the flag) if the captured window was resized.
    // After this, recreate size-dependent D3D resources to the new dimensions.
    bool ConsumeResizeEvent()
    {
        bool expected = true;
        return m_resized.compare_exchange_strong(expected, false,
            std::memory_order_relaxed, std::memory_order_relaxed);
    }

private:
    // Shared setup once a GraphicsCaptureItem exists (window- or monitor-sourced).
    bool StartWithItem(winrt::Windows::Graphics::Capture::GraphicsCaptureItem item, ID3D11Device* device);

    // ---- DXGI Desktop Duplication backend ----
    // targetHwnd == nullptr -> monitor mode (fixed output, no crop).
    // targetHwnd != nullptr -> window mode (crop to SetCropRect's rect, output re-resolved
    // from the window's current monitor every frame).
    bool StartDuplication(HWND targetHwnd, HMONITOR targetMonitor, ID3D11Device* device);
    // (Re)creates m_duplication against the IDXGIOutput1 owning 'monitor'. Called on first
    // start, after DXGI_ERROR_ACCESS_LOST, and whenever the target's monitor changes.
    bool RebuildDuplication(HMONITOR monitor);
    void TeardownDuplication();
    ID3D11ShaderResourceView* AcquireDXGIFrameSRV(ID3D11Device* device);

    // Kendi basina donen yakalama thread'i -- bkz. .cpp'deki uzun aciklama. Ozet: DXGI'nin
    // AcquireNextFrame'i tek-yuvali (kuyruksuz) oldugundan, onu render thread'inden SENKRON
    // cagirmak yakalama hizini render/DLSS-NR'nin kendi GPU maliyetine (o da render thread'in
    // ne kadar sik AcquireNextFrame'e ugrayabildigini belirliyor) baglar -- oyun gercekte daha
    // hizli uretse de biz sadece kendi hizimizda sorup arada kalan gercek kareleri sessizce
    // kaybederiz. WGC zaten kendi arka plan thread'inde (FrameArrived) calisir ve render
    // thread'i sadece SONUCU tuketir; DXGI'i da AYNI modele tasiyoruz: bu thread AcquireNextFrame'i
    // kendi hizinda (yalnizca kDxgiAcquireTimeoutMs'in kendi blocking'iyle sinirli) cagirir,
    // her basarili karede m_dupLatestSRV/m_newFrame/m_frameEvent'i gunceller; render thread
    // (AcquireCurrentFrameSRV) artik hic AcquireNextFrame cagirmiyor, sadece en son sonucu okuyor.
    void DupPollThreadProc();

    // ---- FPS "probe": DXGIDuplication aktifken IN/OUT gostergesindeki giris FPS'ini
    // OLCMEK icin, piksel verisi almadan sadece WGC'nin FrameArrived zamanlamasini kullanan
    // ikincil, hafif bir capture oturumu. Neden gerekli: DXGI Duplication tum masaustunu
    // yakaladigindan ve bizim topmost/opak overlay'imiz hedefin TAM USTUNDE oldugundan,
    // "yeni kompoze kare" sinyali cogunlukla bizim KENDI Present()'imiz tarafindan
    // tetikleniyor -- gercek oyun kare hizindan bagimsiz, sabit/GPU-bagli bir hizda (bkz.
    // kullanici raporlari: oyun 30/60/70 FPS olsun, gosterge hep ~55-57'de kaliyor). WGC'nin
    // CreateForWindow'u ise hedefin REDIRECTION SURFACE'ini dogrudan izole okur -- ekranda ne
    // gorunur oldugundan tamamen bagimsiz, dolayisiyla FrameArrived SADECE hedef GERCEKTEN
    // yeni bir kare urettiginde ateslenir. Bu probe'un tek gorevi RecordFrameArrival'i dogru
    // zamanlarda cagirmak; frame icerigi hic okunmuyor, hemen birakiliyor.
    bool StartFpsProbe(HWND targetHwnd, HMONITOR targetMonitor, ID3D11Device* device);
    void StopFpsProbe();

    // Shared frame-gap/input-FPS bookkeeping (WGC's FrameArrived callback and the DXGI
    // poll path both call this once per genuinely new frame).
    void RecordFrameArrival();

    CaptureBackend m_backend = CaptureBackend::WGC;

    ComPtr<IDXGIOutputDuplication> m_duplication;
    ComPtr<IDXGIOutput1>           m_output;
    HMONITOR                       m_dupMonitor    = nullptr;
    HWND                            m_dupTargetHwnd = nullptr; // nullptr in monitor mode
    bool                            m_dupNeedsRebuild = false; // set after ACCESS_LOST

    // m_duplication/m_output/m_dupMonitor/m_dupNeedsRebuild ve m_cropRect/m_hasCropRect,
    // DupPollThreadProc (yakalama thread'i) tarafindan OKUNUP/YAZILIRKEN, ana/render thread
    // de SetCropRect (App::UpdateOverlayPosition, her kare) ve RebuildDuplication (monitor
    // degisimi) uzerinden bunlara YAZIYOR -- bu kilit olmadan iki thread'in ayni ComPtr'i
    // ayni anda Reset/atama yapmasi (veri yarisi) ya da yakalama thread'inin degismekte olan
    // bir crop rect'i yarim okumasi anlamina gelirdi. AcquireDXGIFrameSRV ve SetCropRect/
    // RebuildDuplication bu kilidi tutar; GPU calismasinin kendisi (CopyResource vb.) kilit
    // ALTINDA degil, sadece paylasilan durum erisimi.
    std::mutex m_dupStateMutex;

    std::thread       m_dupPollThread;
    std::atomic<bool> m_dupPollThreadRun{ false };

    // Yakalama thread'inin urettigi en son SRV -- render thread bunu AcquireCurrentFrameSRV
    // icinde okur (m_newFrame true oldugunda). Alttaki dokunun kendisi rotasyonlu havuzda
    // (m_dupTexture/m_dupSRV) yasiyor; bu sadece "en son hangisi" isaretcisi.
    std::atomic<ID3D11ShaderResourceView*> m_dupLatestSRV{ nullptr };

    // Our own persistent copy of the (possibly cropped) desktop region -- the frame handed
    // back by AcquireNextFrame is released immediately after copying (see .cpp for why).
    // A small ROTATING pool (like WGC's own frame pool) rather than one reused texture: the
    // returned SRV is read downstream by the cross-API (D3D11->D3D12) pipeline for the
    // duration of that render, and reusing a single texture would let the NEXT copy start
    // overwriting it while that read is still in flight (no such race exists for WGC, since
    // each of ITS frames is a distinct pool buffer).
    // Yakalama artik ayri bir thread'de (DupPollThreadProc) render thread'inden bagimsiz
    // hizda dondugu icin, render thread yavas kalirsa yakalama thread'i havuzu daha sik
    // tur atabilir -- 3'ten 5'e cikarmak (WGC'nin kFramePoolBufferCount'uyla ayni), render
    // thread henuz okumamis bir tamponun uzerine yazilma ihtimalini pratikte sifira indirir.
    static constexpr int kDupBufferCount = 5;
    ComPtr<ID3D11Texture2D>          m_dupTexture[kDupBufferCount];
    ComPtr<ID3D11ShaderResourceView> m_dupSRV[kDupBufferCount];
    int                               m_dupBufferIdx = 0;
    DXGI_FORMAT                      m_dupTextureFormat = DXGI_FORMAT_UNKNOWN;
    // Per-SESSION frame counter (NOT a static local -- those persist for the whole process,
    // so a second test run in the same still-running VLSS5.exe would silently inherit the
    // previous session's count and make "frame #200" mean something else entirely).
    uint64_t                         m_dupCopyCount = 0;

    RECT m_cropRect        = {};    // screen coords, window mode only
    bool m_hasCropRect     = false;
    bool m_dupWasOccluded  = false; // edge-triggered occlusion logging (see AcquireDXGIFrameSRV)

    // bkz. StartFpsProbe -- ikincil, salt-zamanlama amacli WGC oturumu (piksel verisi
    // hicbir yerde kullanilmiyor/okunmuyor).
    bool m_probeActive = false;
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem             m_probeItem{ nullptr };
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice     m_probeWinrtDevice{ nullptr };
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool      m_probeFramePool{ nullptr };
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession          m_probeSession{ nullptr };
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::FrameArrived_revoker
                                                                        m_probeFrameArrivedRevoker;

    struct SRVCacheEntry
    {
        ID3D11Texture2D* texture = nullptr;
        ComPtr<ID3D11ShaderResourceView> srv;
    };
    static constexpr size_t kMaxCachedSRVs = 8;
    SRVCacheEntry m_srvCache[kMaxCachedSRVs];

    winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame m_currentFrame{ nullptr };
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame m_nextFrame{ nullptr };

    bool   m_active     = false;
    HANDLE m_frameEvent = nullptr;
    int    m_width      = 0;
    int    m_height     = 0;

    winrt::Windows::Graphics::SizeInt32 m_captureSize{};

    winrt::Windows::Graphics::Capture::GraphicsCaptureItem             m_item{ nullptr };
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice     m_winrtDevice{ nullptr };
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool      m_framePool{ nullptr };
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession           m_session{ nullptr };
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::FrameArrived_revoker
                                                                        m_frameArrivedRevoker;
    // WGC oturumu kendi icinden (pencere kapandi, GPU/DPI degisti, vb.) kapanirsa ates alir --
    // bkz. IsSessionBroken() aciklamasi.
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem::Closed_revoker
                                                                        m_itemClosedRevoker;
    ComPtr<ID3D11Device> m_device;

    // FrameArrived callback'i (WGC'nin kendi free-threaded thread pool'unda calisir) ile Stop()
    // (render/ana thread) arasinda paylasilir. Eskiden Stop() sadece revoke()+Sleep(15) ile
    // "callback artik calismiyordur" varsayiyordu -- revoke() SADECE gelecekteki cagrilari
    // engeller, O AN calismakta olan bir cagriyi beklemez. Callback govdesi (birkac atomic yazma
    // + SetEvent + RecordFrameArrival) bu kilidi tutarak calisir; Stop() m_frameEvent'i kapatip
    // m_session/m_framePool'u yok etmeden once ayni kilidi alir -- boylece teardown olasiliksal
    // degil deterministik olur.
    std::mutex m_frameCallbackMutex;

    // Bkz. IsSessionBroken(). StartWithItem() basariyla kurulduktan sonra false'a resetlenir.
    std::atomic<bool> m_sessionBroken{ false };

    std::atomic<bool> m_newFrame{ false };
    std::atomic<bool> m_resized { false };

    // ---- WGC frame-gap health tracking (thread-safe via interlocked / only read on render thread) ----
    LARGE_INTEGER m_lastFrameArrivalTime   = {};
    LARGE_INTEGER m_lastGapLogTime         = {};
    double        m_lastFrameGapMs         = 0.0;
    double        m_maxFrameGapMs          = 0.0;
    double        m_sumFrameGapMs          = 0.0;
    uint64_t      m_frameGapSamples        = 0;
    
    // FPS tracking for WGC
    int           m_currentInputFps        = 0;
    int           m_inputFpsFrameCount     = 0;
    LARGE_INTEGER m_inputFpsLastTime       = {};

    // Rolling window for frame-gap jitter (variance / stddev)
    static constexpr int kJitterWindow = 120;
    double        m_gapHistory[kJitterWindow] = {};
    int           m_gapHistoryIdx          = 0;

    // Consecutive null-frame counter (TryGetNextFrame returns nullptr → session may be dead)
    uint32_t      m_nullFrameStreak        = 0;
    static constexpr uint32_t kNullFrameWarnThreshold = 60; // warn after ~60 consecutive empty polls

public:
    // Called from the render thread approximately every 5 s to emit a summary and reset accumulators.
    void FlushCaptureStats();
};
