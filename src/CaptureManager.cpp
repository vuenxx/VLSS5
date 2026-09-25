#include "CaptureManager.h"
#include "WindowEnumerator.h"

// 5 frame pool buffers allow GPU pipelining at high refresh rates (120-240 FPS / Frame Gen) without dropping frames
static constexpr int32_t kFramePoolBufferCount = 5;

// DXGI Desktop Duplication has no FrameArrived-style callback, so it's polled from its own
// dedicated background thread (CaptureManager::DupPollThreadProc) instead of the render
// thread -- this timeout is simply how long that thread blocks per idle poll before checking
// whether it's still supposed to be running (m_dupPollThreadRun). It intentionally does NOT
// pace the render thread anymore: coupling capture polling to render throughput was exactly
// the bug that made "IN" FPS track our own GPU-bound render rate instead of the source's real
// rate (see DupPollThreadProc's doc comment in CaptureManager.h).
static constexpr UINT kDxgiAcquireTimeoutMs = 4;

// -----------------------------------------------------------------------
// DEBUG ONLY: dumps a D3D11 B8G8R8A8_UNORM texture to a 32-bit BMP file next to the exe,
// so a captured frame can be visually inspected directly instead of guessed at from
// telemetry. Not wired to any UI -- called from specific frame counts in AcquireDXGIFrameSRV.
// -----------------------------------------------------------------------
static void DebugDumpTextureToBmp(ID3D11Device* device, ID3D11DeviceContext* ctx,
    ID3D11Texture2D* tex, const wchar_t* filename)
{
    if (!device || !ctx || !tex) return;

    D3D11_TEXTURE2D_DESC desc = {};
    tex->GetDesc(&desc);
    if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM)
    {
        DLSS_Log("[Capture] DEBUG dump atlandi: beklenmeyen format %d", (int)desc.Format);
        return;
    }

    D3D11_TEXTURE2D_DESC sd = desc;
    sd.Usage          = D3D11_USAGE_STAGING;
    sd.BindFlags      = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags      = 0;

    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&sd, nullptr, &staging)))
    {
        DLSS_Log("[Capture] DEBUG dump: staging texture olusturulamadi");
        return;
    }
    ctx->CopyResource(staging.Get(), tex);

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
    {
        DLSS_Log("[Capture] DEBUG dump: Map basarisiz");
        return;
    }

    const UINT w = desc.Width, h = desc.Height;
    const UINT rowBytes = w * 4;
    const DWORD pixelDataSize = rowBytes * h;

    BITMAPFILEHEADER bfh = {};
    bfh.bfType    = 0x4D42; // 'BM'
    bfh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    bfh.bfSize    = bfh.bfOffBits + pixelDataSize;

    BITMAPINFOHEADER bih = {};
    bih.biSize          = sizeof(BITMAPINFOHEADER);
    bih.biWidth         = static_cast<LONG>(w);
    bih.biHeight        = static_cast<LONG>(h); // positive = bottom-up
    bih.biPlanes        = 1;
    bih.biBitCount      = 32;
    bih.biCompression   = BI_RGB;
    bih.biSizeImage     = pixelDataSize;

    wchar_t exeDir[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exeDir, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(exeDir, L'\\');
    if (lastSlash) *(lastSlash + 1) = L'\0';
    std::wstring path = std::wstring(exeDir) + filename;

    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        ctx->Unmap(staging.Get(), 0);
        DLSS_Log("[Capture] DEBUG dump: dosya acilamadi: %ls", path.c_str());
        return;
    }

    DWORD written = 0;
    WriteFile(hFile, &bfh, sizeof(bfh), &written, nullptr);
    WriteFile(hFile, &bih, sizeof(bih), &written, nullptr);

    // BMP rows are stored bottom-up; write source rows in reverse order.
    std::vector<uint8_t> rowBuf(rowBytes);
    const uint8_t* srcBase = static_cast<const uint8_t*>(mapped.pData);
    for (LONG y = static_cast<LONG>(h) - 1; y >= 0; --y)
    {
        const uint8_t* srcRow = srcBase + static_cast<size_t>(y) * mapped.RowPitch;
        memcpy(rowBuf.data(), srcRow, rowBytes);
        WriteFile(hFile, rowBuf.data(), rowBytes, &written, nullptr);
    }

    CloseHandle(hFile);
    ctx->Unmap(staging.Get(), 0);

    DLSS_Log("[Capture] DEBUG dump yazildi: %ls (%ux%u)", path.c_str(), w, h);
}

// -----------------------------------------------------------------------
// Start
// -----------------------------------------------------------------------
// Bayat-korumali giris FPS'i (bkz. CaptureManager.h)
int CaptureManager::GetInputFpsFresh(double maxAgeSeconds) const
{
    if (m_lastFrameArrivalTime.QuadPart == 0) return 0;

    LARGE_INTEGER now = {}, freq = {};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    if (freq.QuadPart == 0) return m_currentInputFps;

    const double age =
        static_cast<double>(now.QuadPart - m_lastFrameArrivalTime.QuadPart) /
        static_cast<double>(freq.QuadPart);

    return (age > maxAgeSeconds) ? 0 : m_currentInputFps;
}

void CaptureManager::ResetInputFpsWindow()
{
    m_inputFpsFrameCount   = 0;
    m_inputFpsLastTime     = {};
}

bool CaptureManager::Start(HWND targetHwnd, ID3D11Device* device, CaptureBackend backend)
{
    Stop(); // clean up any prior session

    if (backend == CaptureBackend::DXGIDuplication)
    {
        HMONITOR mon = MonitorFromWindow(targetHwnd, MONITOR_DEFAULTTONEAREST);
        if (StartDuplication(targetHwnd, mon, device))
        {
            m_backend = CaptureBackend::DXGIDuplication;
            return true;
        }
        DLSS_Log("[Capture] DXGI Desktop Duplication baslatilamadi (pencere modu), WGC'ye dusuluyor");
        Stop(); // clean up any partial duplication state before falling back
    }

    // --- Create GraphicsCaptureItem for the target HWND ---
    //
    // ONEMLI: get_activation_factory/CreateForWindow ve asagidaki
    // StartWithItem() ADIM ADIM winrt::hresult_error FIRLATABILIR (ozellikle
    // hedef pencere tam o anda focus/aktivasyon gecisi yasiyorsa -- WGC bu
    // durumda capture'i reddedip exception atabiliyor). Once buradaki
    // HRESULT kontrolu (FAILED(hr)) bu ihtimallerin YALNIZCA bir kismini
    // yakaliyordu; gercek bir throw hicbir yerde try/catch'e sarilmadigi
    // icin App::StartOverlayCommon'a kadar cikip TUM UYGULAMAYI cokertiyordu
    // (crash dump: winrt::hresult_error, tam da "pencereyi odaklayinca"
    // BASLAT'a basilan an). Cozum: tum WGC baslatma zincirini try/catch'e al,
    // hata durumunda sessizce false don -- caginin zaten bekledigi "capture
    // baslatilamadi" yolu.
    try
    {
        auto factory = winrt::get_activation_factory<
            winrt::Windows::Graphics::Capture::GraphicsCaptureItem,
            IGraphicsCaptureItemInterop>();

        winrt::Windows::Graphics::Capture::GraphicsCaptureItem item{ nullptr };
        HRESULT hr = factory->CreateForWindow(
            targetHwnd,
            winrt::guid_of<winrt::Windows::Graphics::Capture::GraphicsCaptureItem>(),
            winrt::put_abi(item));

        if (FAILED(hr) || !item) return false;

        m_backend = CaptureBackend::WGC;
        return StartWithItem(item, device);
    }
    catch (const winrt::hresult_error& ex)
    {
        DLSS_Log("[Capture] WGC (pencere) baslatilirken winrt istisnasi: 0x%08X (%ls)",
            (unsigned)ex.code(), ex.message().c_str());
        return false;
    }
    catch (...)
    {
        DLSS_Log("[Capture] WGC (pencere) baslatilirken bilinmeyen istisna.");
        return false;
    }
}

bool CaptureManager::StartMonitor(HMONITOR targetMonitor, ID3D11Device* device, CaptureBackend backend)
{
    Stop(); // clean up any prior session

    if (backend == CaptureBackend::DXGIDuplication)
    {
        if (StartDuplication(nullptr, targetMonitor, device))
        {
            m_backend = CaptureBackend::DXGIDuplication;
            return true;
        }
        DLSS_Log("[Capture] DXGI Desktop Duplication baslatilamadi (monitor modu), WGC'ye dusuluyor");
        Stop();
    }

    // --- Create GraphicsCaptureItem for the target monitor ("whole screen" mode) ---
    // Bkz. Start()'taki ayni konudaki not: bu zincir winrt::hresult_error
    // firlatabilir, try/catch'siz uygulamayi cokertir.
    try
    {
        auto factory = winrt::get_activation_factory<
            winrt::Windows::Graphics::Capture::GraphicsCaptureItem,
            IGraphicsCaptureItemInterop>();

        winrt::Windows::Graphics::Capture::GraphicsCaptureItem item{ nullptr };
        HRESULT hr = factory->CreateForMonitor(
            targetMonitor,
            winrt::guid_of<winrt::Windows::Graphics::Capture::GraphicsCaptureItem>(),
            winrt::put_abi(item));

        if (FAILED(hr) || !item) return false;

        m_backend = CaptureBackend::WGC;
        return StartWithItem(item, device);
    }
    catch (const winrt::hresult_error& ex)
    {
        DLSS_Log("[Capture] WGC (monitor) baslatilirken winrt istisnasi: 0x%08X (%ls)",
            (unsigned)ex.code(), ex.message().c_str());
        return false;
    }
    catch (...)
    {
        DLSS_Log("[Capture] WGC (monitor) baslatilirken bilinmeyen istisna.");
        return false;
    }
}

// -----------------------------------------------------------------------
// StartDuplication / RebuildDuplication / TeardownDuplication / SetCropRect /
// AcquireDXGIFrameSRV — DXGI Desktop Duplication backend.
// -----------------------------------------------------------------------
bool CaptureManager::StartDuplication(HWND targetHwnd, HMONITOR targetMonitor, ID3D11Device* device)
{
    if (!device || !targetMonitor) return false;

    m_device        = device;
    m_dupTargetHwnd = targetHwnd; // nullptr => monitor mode, no crop
    m_hasCropRect   = false;
    m_cropRect      = {};
    m_dupCopyCount  = 0;

    {
        std::lock_guard<std::mutex> lock(m_dupStateMutex);
        if (!RebuildDuplication(targetMonitor))
            return false;
    }

    m_frameEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    if (!m_dupTargetHwnd)
    {
        // Monitor mode: size is fixed to the output's full desktop bounds right away.
        DXGI_OUTPUT_DESC desc = {};
        m_output->GetDesc(&desc);
        m_width  = desc.DesktopCoordinates.right  - desc.DesktopCoordinates.left;
        m_height = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;
    }
    // Window mode: m_width/m_height stay 0 until the caller's first SetCropRect() call
    // (App::StartOverlayCommon calls it immediately after Start() succeeds).

    // Artik gercek bir olay-tabanli model: m_newFrame baslangicta false, DupPollThreadProc
    // (bkz. header) genuine bir kare yakaladiginda true yapip m_frameEvent'i SetEvent eder --
    // WGC'nin FrameArrived'iyla ayni desen. Eskiden burada kalici true birakip AcquireNextFrame'i
    // render thread'inden SENKRON cagirmak, yakalama hizini render/DLSS-NR'nin GPU maliyetine
    // bagliyordu (oyun daha hizli uretse de biz sadece kendi hizimizda sorabiliyorduk).
    m_newFrame.store(false, std::memory_order_relaxed);
    m_dupLatestSRV.store(nullptr, std::memory_order_relaxed);
    m_active = true;

    m_dupPollThreadRun.store(true, std::memory_order_relaxed);
    m_dupPollThread = std::thread(&CaptureManager::DupPollThreadProc, this);

    // Bkz. header: WGC tabanli, salt-zamanlama amacli yedek FPS probe'u da baslatiyoruz --
    // yakalama thread'i artik dogru hizda calissa da, bu ucuz bir ek dogrulama/yedek katmani.
    StartFpsProbe(targetHwnd, targetMonitor, device);

    return true;
}

bool CaptureManager::RebuildDuplication(HMONITOR monitor)
{
    if (!m_device || !monitor) return false;

    m_duplication.Reset();
    m_output.Reset();

    ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(m_device->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) return false;

    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgiDevice->GetParent(IID_PPV_ARGS(&adapter)))) return false;

    for (UINT i = 0; ; ++i)
    {
        ComPtr<IDXGIOutput> output;
        HRESULT hr = adapter->EnumOutputs(i, &output);
        if (hr == DXGI_ERROR_NOT_FOUND || FAILED(hr) || !output) break;

        DXGI_OUTPUT_DESC desc = {};
        if (FAILED(output->GetDesc(&desc)) || desc.Monitor != monitor) continue;

        ComPtr<IDXGIOutput1> output1;
        if (FAILED(output.As(&output1))) return false;

        // DLSS-NR'nin paylasimli giris dokusu sabit DXGI_FORMAT_B8G8R8A8_UNORM olarak
        // olusturuluyor (bkz. D3D12Interop::CreateCrossAdapterBridge/Init) ve her kare
        // duz CopyResource ile bu dokuya kopyalaniyor -- format uyusmazliginda CopyResource
        // sessizce hicbir sey yapmiyor (exception yok, D3D11 debug-layer uyarisi var), yani
        // giris dokusu asla guncellenmiyor ve DLSS5 hep eski kareyi isleyip ekran donmus gibi
        // kaliyor. WGC bu sorunu hic yasamaz cunku frame pool'u zaten B8G8R8A8UIntNormalized'a
        // sabitliyor. Masaustu HDR/Advanced Color aciksa DuplicateOutput'un dondurdugu format
        // R16G16B16A16_FLOAT gibi baska bir sey olabilir -- IDXGIOutput5::DuplicateOutput1 ile
        // desteklenen format listesinden acikca B8G8R8A8_UNORM istiyoruz; composition engine
        // gerekli tone-map/donusumu kendi yapiyor, WGC'nin garantisiyle ayni sonuc.
        ComPtr<IDXGIOutputDuplication> dup;
        ComPtr<IDXGIOutput5> output5;
        HRESULT dhr = E_NOINTERFACE;
        if (SUCCEEDED(output.As(&output5)))
        {
            static constexpr DXGI_FORMAT kPreferredFormats[] = { DXGI_FORMAT_B8G8R8A8_UNORM };
            dhr = output5->DuplicateOutput1(m_device.Get(), 0,
                static_cast<UINT>(_countof(kPreferredFormats)), kPreferredFormats, &dup);
            if (FAILED(dhr))
            {
                DLSS_Log("[Capture] DuplicateOutput1 (B8G8R8A8_UNORM) basarisiz (hr=0x%08X) -- "
                         "eski DuplicateOutput yoluna dusuluyor (HDR ekranlarda format uyumsuzlugu "
                         "riski var)", static_cast<unsigned>(dhr));
            }
        }
        if (FAILED(dhr))
        {
            dhr = output1->DuplicateOutput(m_device.Get(), &dup);
        }
        if (FAILED(dhr))
        {
            DLSS_Log("[Capture] DuplicateOutput basarisiz (hr=0x%08X) -- yakalama GPU'su bu "
                     "monitoru suren adapter olmayabilir, ya da baska bir uygulama zaten bu "
                     "ciktiyi kopyaliyor olabilir", static_cast<unsigned>(dhr));
            return false;
        }

        m_output          = output1;
        m_duplication     = dup;
        m_dupMonitor      = monitor;
        m_dupNeedsRebuild = false;
        return true;
    }

    DLSS_Log("[Capture] Hedef monitor icin IDXGIOutput bulunamadi");
    return false;
}

void CaptureManager::TeardownDuplication()
{
    // Once ONCE thread'i durdur -- m_duplication asagida Reset edilecek, thread hala
    // AcquireNextFrame cagiriyor olsaydi bu use-after-free/crash olurdu.
    m_dupPollThreadRun.store(false, std::memory_order_relaxed);
    if (m_dupPollThread.joinable())
        m_dupPollThread.join();
    m_dupLatestSRV.store(nullptr, std::memory_order_relaxed);

    StopFpsProbe();

    m_duplication.Reset();
    m_output.Reset();
    for (int i = 0; i < kDupBufferCount; ++i)
    {
        m_dupSRV[i].Reset();
        m_dupTexture[i].Reset();
    }
    m_dupBufferIdx      = 0;
    m_dupMonitor        = nullptr;
    m_dupTargetHwnd     = nullptr;
    m_dupTextureFormat  = DXGI_FORMAT_UNKNOWN;
    m_hasCropRect       = false;
    m_cropRect          = {};
    m_dupNeedsRebuild   = false;
    m_dupWasOccluded    = false;
}

bool CaptureManager::SetCropRect(const RECT& screenRect)
{
    if (m_backend != CaptureBackend::DXGIDuplication || !m_dupTargetHwnd) return false;

    // DupPollThreadProc (yakalama thread'i) AcquireDXGIFrameSRV icinde ayni durumu
    // (m_cropRect/m_hasCropRect/m_duplication/m_dupTexture...) okuyup yaziyor -- bkz.
    // header'daki m_dupStateMutex aciklamasi.
    std::lock_guard<std::mutex> lock(m_dupStateMutex);

    const int w = screenRect.right  - screenRect.left;
    const int h = screenRect.bottom - screenRect.top;
    if (w <= 0 || h <= 0) return false;

    const bool sizeChanged = !m_hasCropRect || (w != m_width) || (h != m_height);

    const bool originChanged = !m_hasCropRect ||
        (screenRect.left != m_cropRect.left) || (screenRect.top != m_cropRect.top);

    m_cropRect    = screenRect;
    m_hasCropRect = true;

    if (sizeChanged || originChanged)
    {
        DLSS_Log("[Capture] SetCropRect: ekran=(%ld,%ld)-(%ld,%ld) boyut=%dx%d (onceki=%dx%d, boyut_degisti=%d, konum_degisti=%d)",
            screenRect.left, screenRect.top, screenRect.right, screenRect.bottom,
            w, h, m_width, m_height, sizeChanged ? 1 : 0, originChanged ? 1 : 0);
    }

    if (sizeChanged)
    {
        m_width  = w;
        m_height = h;
        m_resized.store(true, std::memory_order_relaxed);

        // Force AcquireDXGIFrameSRV to recreate the persistent copy textures at the new size.
        for (int i = 0; i < kDupBufferCount; ++i)
        {
            m_dupTexture[i].Reset();
            m_dupSRV[i].Reset();
        }
    }

    // Target moved to a different monitor -> duplication must be rebound to that output.
    HMONITOR mon = MonitorFromWindow(m_dupTargetHwnd, MONITOR_DEFAULTTONEAREST);
    if (mon && mon != m_dupMonitor)
    {
        if (!RebuildDuplication(mon))
            m_dupNeedsRebuild = true; // AcquireDXGIFrameSRV retries next tick
    }

    return true;
}

ID3D11ShaderResourceView* CaptureManager::AcquireDXGIFrameSRV(ID3D11Device* device)
{
    // bkz. header'daki m_dupStateMutex aciklamasi -- SetCropRect (render/ana thread) ile
    // ayni paylasilan durumu (m_duplication, m_cropRect, m_dupTexture havuzu...) korur.
    // AcquireNextFrame'in kendi kDxgiAcquireTimeoutMs (4ms) blogu bu kilit ALTINDA olur;
    // SetCropRect bu yuzden en fazla ~4ms bekleyebilir -- UpdateOverlayPosition zaten kendi
    // ici degisim/100ms throttle'i oldugundan bu onemsiz bir maliyettir.
    std::lock_guard<std::mutex> lock(m_dupStateMutex);

    if (m_dupNeedsRebuild)
    {
        HMONITOR mon = m_dupTargetHwnd ? MonitorFromWindow(m_dupTargetHwnd, MONITOR_DEFAULTTONEAREST) : m_dupMonitor;
        RebuildDuplication(mon);
    }
    if (!m_duplication) return nullptr;

    // Window mode: nothing to crop to yet (first SetCropRect() hasn't landed).
    if (m_dupTargetHwnd && !m_hasCropRect) return nullptr;

    DXGI_OUTDUPL_FRAME_INFO frameInfo = {};
    ComPtr<IDXGIResource> desktopResource;
    HRESULT hr = m_duplication->AcquireNextFrame(kDxgiAcquireTimeoutMs, &frameInfo, &desktopResource);

    if (hr == DXGI_ERROR_WAIT_TIMEOUT)
        return nullptr; // no new frame this tick -- normal, desktop hasn't changed

    if (hr == DXGI_ERROR_ACCESS_LOST)
    {
        DLSS_Log("[Capture] DXGI Duplication erisimi kaybedildi "
                 "(cikis modu degisti / kilit ekrani / baska bir uygulama devraldi) -- yeniden kuruluyor");
        m_duplication.Reset();
        m_dupNeedsRebuild = true;

        // Havuzdaki tamponlari da zorla yeniden yaratiyoruz: ACCESS_LOST sonrasi yeni
        // duplication oturumunun dondurdugu dokular onceki oturumla ayni GPU tahsisi
        // olmayabilir; eski tamponlari oldugu gibi tutup uzerine kismi kopya yapmaya
        // devam etmek (needsRecreate yalnizca BOYUT/format degisimini yakaliyor, oturum
        // degisimini degil) bozuk/tutarsiz icerige yol acabilir.
        for (int i = 0; i < kDupBufferCount; ++i)
        {
            m_dupTexture[i].Reset();
            m_dupSRV[i].Reset();
        }
        m_dupTextureFormat = DXGI_FORMAT_UNKNOWN;
        return nullptr;
    }

    if (FAILED(hr) || !desktopResource) return nullptr;

    // Occlusion safety (window-crop mode only): unlike WGC's CreateForWindow, duplication
    // has no per-window isolation -- it captures the whole output, so a foreign window sitting
    // over the target's screen rect (Discord toast, another app dragged on top, etc.) would
    // otherwise leak into the crop. Skip updating this tick and keep presenting the last good
    // frame instead; the duplication frame still has to be released every tick regardless.
    if (m_dupTargetHwnd && WindowEnumerator::IsOccluded(m_dupTargetHwnd, m_cropRect))
    {
        if (!m_dupWasOccluded)
        {
            DLSS_Log("[Capture] Hedef pencere yabanci bir pencere tarafindan ortuldu -- "
                     "kirli kare atlaniyor (son iyi kare ekranda kaliyor)");
            m_dupWasOccluded = true;
        }
        m_duplication->ReleaseFrame();
        return nullptr;
    }
    if (m_dupWasOccluded)
    {
        DLSS_Log("[Capture] Hedef pencere artik ortulmemis -- yakalama devam ediyor");
        m_dupWasOccluded = false;
    }

    ComPtr<ID3D11Texture2D> desktopTexture;
    hr = desktopResource.As(&desktopTexture);
    if (FAILED(hr) || !desktopTexture)
    {
        m_duplication->ReleaseFrame();
        return nullptr;
    }

    D3D11_TEXTURE2D_DESC srcDesc = {};
    desktopTexture->GetDesc(&srcDesc);

    // DuplicateOutput1(B8G8R8A8_UNORM) yukarida acikca istendi (bkz. RebuildDuplication);
    // eger surucu/OS bunu yine de gormezden gelip baska bir format donduruyorsa (ornegin
    // DuplicateOutput1 desteklenmiyorsa eski DuplicateOutput yoluna dusulmus olabilir),
    // DLSS5'in sabit-BGRA8 giris dokusuna yapilan CopyResource sessizce basarisiz olup
    // ekranin donmus gibi kalmasina yol acar -- bunu tespit edilebilir kilmak icin bir kere logla.
    if (srcDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM)
    {
        static bool s_formatWarnLogged = false;
        if (!s_formatWarnLogged)
        {
            s_formatWarnLogged = true;
            DLSS_Log("[Capture] UYARI: DXGI Duplication format=%d (B8G8R8A8_UNORM=%d degil) -- "
                     "DLSS5 (Feature 18) acikken bu, CopyResource format uyumsuzlugu nedeniyle "
                     "giris dokusunun guncellenmemesine ve ekranin donmus gibi kalmasina yol acabilir.",
                     (int)srcDesc.Format, (int)DXGI_FORMAT_B8G8R8A8_UNORM);
        }
    }

    if (m_width <= 0 || m_height <= 0)
    {
        m_duplication->ReleaseFrame();
        return nullptr;
    }

    // Rotate to the next pool buffer for THIS frame (matches WGC's own frame-pool pattern --
    // see the kDupBufferCount comment in CaptureManager.h for why a single reused texture
    // is unsafe here).
    m_dupBufferIdx = (m_dupBufferIdx + 1) % kDupBufferCount;
    ComPtr<ID3D11Texture2D>&          dstTexture = m_dupTexture[m_dupBufferIdx];
    ComPtr<ID3D11ShaderResourceView>& dstSRV     = m_dupSRV[m_dupBufferIdx];

    // (Re)create this buffer if missing or the format changed
    // (format tracks the actual desktop swapchain format -- BGRA8 SDR or FP16 HDR/WCG).
    bool needsRecreate = !dstTexture || m_dupTextureFormat != srcDesc.Format;
    if (dstTexture && !needsRecreate)
    {
        D3D11_TEXTURE2D_DESC cur = {};
        dstTexture->GetDesc(&cur);
        if ((int)cur.Width != m_width || (int)cur.Height != m_height) needsRecreate = true;
    }

    if (needsRecreate)
    {
        dstTexture.Reset();
        dstSRV.Reset();

        D3D11_TEXTURE2D_DESC td = {};
        td.Width            = static_cast<UINT>(m_width);
        td.Height           = static_cast<UINT>(m_height);
        td.MipLevels        = 1;
        td.ArraySize        = 1;
        td.Format           = srcDesc.Format;
        td.SampleDesc.Count = 1;
        td.Usage            = D3D11_USAGE_DEFAULT;
        td.BindFlags        = D3D11_BIND_SHADER_RESOURCE;

        if (FAILED(device->CreateTexture2D(&td, nullptr, &dstTexture)))
        {
            m_duplication->ReleaseFrame();
            return nullptr;
        }
        m_dupTextureFormat = srcDesc.Format;

        D3D11_SHADER_RESOURCE_VIEW_DESC srvd = {};
        srvd.Format                    = srcDesc.Format;
        srvd.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvd.Texture2D.MipLevels       = 1;
        srvd.Texture2D.MostDetailedMip = 0;

        if (FAILED(device->CreateShaderResourceView(dstTexture.Get(), &srvd, &dstSRV)))
        {
            dstTexture.Reset();
            m_duplication->ReleaseFrame();
            return nullptr;
        }
    }

    ComPtr<ID3D11DeviceContext> ctx;
    device->GetImmediateContext(&ctx);

    if (!m_dupTargetHwnd)
    {
        // Monitor mode: sizes match exactly, straight copy.
        ctx->CopyResource(dstTexture.Get(), desktopTexture.Get());
    }
    else
    {
        // Window mode: crop rect is in virtual-desktop screen coords; the acquired texture's
        // origin is this output's own top-left, so offset by the output's desktop coordinates.
        DXGI_OUTPUT_DESC outDesc = {};
        m_output->GetDesc(&outDesc);

        LONG left = m_cropRect.left - outDesc.DesktopCoordinates.left;
        LONG top  = m_cropRect.top  - outDesc.DesktopCoordinates.top;

        // Clamp to the output's own bounds (window can be partially off-screen while dragging).
        LONG clampedLeft = std::max<LONG>(0, std::min<LONG>(left, static_cast<LONG>(srcDesc.Width)));
        LONG clampedTop  = std::max<LONG>(0, std::min<LONG>(top,  static_cast<LONG>(srcDesc.Height)));
        LONG availW = std::min<LONG>(m_width,  static_cast<LONG>(srcDesc.Width)  - clampedLeft);
        LONG availH = std::min<LONG>(m_height, static_cast<LONG>(srcDesc.Height) - clampedTop);

        ++m_dupCopyCount;
        const bool verbose = (m_dupCopyCount <= 5 || (m_dupCopyCount % 300 == 0));
        if (verbose)
        {
            DLSS_Log("[Capture] DXGI crop #%llu: output=(%ld,%ld) src=%ux%u fmt=%d | crop_ekran=(%ld,%ld) "
                     "-> yerel=(%ld,%ld) clamp=(%ld,%ld) avail=%ldx%ld m_wh=%dx%d dstBuf=%d",
                m_dupCopyCount,
                outDesc.DesktopCoordinates.left, outDesc.DesktopCoordinates.top,
                srcDesc.Width, srcDesc.Height, (int)srcDesc.Format,
                m_cropRect.left, m_cropRect.top, left, top,
                clampedLeft, clampedTop, availW, availH, m_width, m_height, m_dupBufferIdx);
        }

        if (availW > 0 && availH > 0)
        {
            D3D11_BOX srcBox = {};
            srcBox.left   = static_cast<UINT>(clampedLeft);
            srcBox.top    = static_cast<UINT>(clampedTop);
            srcBox.front  = 0;
            srcBox.right  = static_cast<UINT>(clampedLeft + availW);
            srcBox.bottom = static_cast<UINT>(clampedTop  + availH);
            srcBox.back   = 1;

            // Only the top-left availW x availH region is refreshed; if the window is
            // partially off-screen the remainder keeps its last-known contents rather than
            // failing outright -- a cosmetic edge case, not a correctness one.
            ctx->CopySubresourceRegion(dstTexture.Get(), 0, 0, 0, 0,
                desktopTexture.Get(), 0, &srcBox);
        }
        else if (verbose)
        {
            DLSS_Log("[Capture] DXGI crop #%llu: availW/H <= 0, kopya ATLANDI", m_dupCopyCount);
        }

        // NOT: burada eskiden ilk birkac karede otomatik BMP debug dump'i tetiklenirdi
        // (DebugDumpTextureToBmp). Bu artik yakalama thread'inden cagrildigi icin KALDIRILDI:
        // Map(D3D11_MAP_READ) GPU'yu flush edip senkron bekliyor ve WriteFile diske yaziyor --
        // ikisi de m_dupStateMutex TUTULURKEN oluyordu, bu da render thread'in ayni paylasilan
        // D3D11 context'ini kullanmasini (ve SetCropRect'i) o sure boyunca fiilen durduruyordu.
        // Fonksiyon hala mevcut, gerekirse elle cagrilabilir.
    }

    m_duplication->ReleaseFrame();

    // NOT: LastPresentTime tabanli "kendi kendini tetikledi mi" filtresi denenmisti ama
    // yanlis sonuc verdi (IN FPS gercekte oldugundan cok daha DUSUK gorunmeye basladi) --
    // DWM'nin WDA_EXCLUDEFROMCAPTURE'li pencerelerde capture-composite'i ne zaman yeniledigi
    // varsayimimiz dogru degilmis. Bunun yerine artik ayri bir WGC "FPS probe" oturumu var
    // (bkz. StartFpsProbe) -- o calisiyorsa GERCEK giris FPS zamanlamasini o veriyor, burada
    // ikinci kez (ve yanlis kaynaktan) sayilmasin diye atlaniyor. Probe basarisiz olduysa
    // (m_probeActive=false) eski yaklasik davranisa geri donuyoruz -- hic olcum olmamasindan iyi.
    if (!m_probeActive)
        RecordFrameArrival();

    // Yakalama thread'inden render thread'e teslim: en son SRV'yi yayinla, "yeni kare var"
    // bayragini kaldir ve WGC'yle ayni desende event'i tetikle (App::Run'daki bekleme dali
    // artik DXGI icin de gercekten calisiyor).
    ID3D11ShaderResourceView* result = dstSRV.Get();
    m_dupLatestSRV.store(result, std::memory_order_release);
    m_newFrame.store(true, std::memory_order_release);
    if (m_frameEvent)
        SetEvent(m_frameEvent);

    return result;
}

// -----------------------------------------------------------------------
// DupPollThreadProc — bkz. header. Render thread'inden bagimsiz calisir, AMA tamamen
// serbest de dondurulemez: overlay'imiz topmost+opak oldugundan (bkz. proje gecmisi),
// AcquireNextFrame bazi durumlarda ARDI ARDINA "yeni kare" dondurebiliyor (timeout'a hic
// girmeden) -- bu durumda bu dongu throttle'siz calisirsa bir CPU cekirdegini kilitleyip
// PAYLASILAN D3D11 immediate context'i (render thread'in DLSS-NR/Present icin de kullandigi
// AYNI context) surekli isgal ediyor, render thread'i acil bekletiyor (gozlemlenen sonuc:
// render FPS ~1-2'ye dustu). 1ms'lik acik bir uyku, dongunun ~1000Hz'i asmasini engelliyor --
// bu, herhangi bir gercekci ekran/oyun hizini yakalamak icin fazlasiyla yeterli, ama CPU
// cekirdegini ve context kilidini duzenli araliklarla birakmayi garantiliyor.
// -----------------------------------------------------------------------
void CaptureManager::DupPollThreadProc()
{
    ComPtr<ID3D11Device> device = m_device; // sabit kopya -- Stop() sirasinda m_device nullPtr'a donebilir
    while (m_dupPollThreadRun.load(std::memory_order_relaxed))
    {
        AcquireDXGIFrameSRV(device.Get());
        Sleep(1);
    }
}

// -----------------------------------------------------------------------
// StartFpsProbe / StopFpsProbe — bkz. header. Piksel verisi ASLA okunmuyor; sadece
// FrameArrived'in zamanlamasi RecordFrameArrival'a besleniyor.
// -----------------------------------------------------------------------
bool CaptureManager::StartFpsProbe(HWND targetHwnd, HMONITOR targetMonitor, ID3D11Device* device)
{
    if (!device) return false;

    try
    {
        auto factory = winrt::get_activation_factory<
            winrt::Windows::Graphics::Capture::GraphicsCaptureItem,
            IGraphicsCaptureItemInterop>();

        winrt::Windows::Graphics::Capture::GraphicsCaptureItem item{ nullptr };
        HRESULT hr = targetHwnd
            ? factory->CreateForWindow(
                  targetHwnd,
                  winrt::guid_of<winrt::Windows::Graphics::Capture::GraphicsCaptureItem>(),
                  winrt::put_abi(item))
            : factory->CreateForMonitor(
                  targetMonitor,
                  winrt::guid_of<winrt::Windows::Graphics::Capture::GraphicsCaptureItem>(),
                  winrt::put_abi(item));
        if (FAILED(hr) || !item) return false;

        ComPtr<IDXGIDevice> dxgiDevice;
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) return false;

        winrt::com_ptr<IInspectable> inspectable;
        if (FAILED(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.Get(), inspectable.put())))
            return false;

        m_probeWinrtDevice = inspectable.as<
            winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();
        m_probeItem = item;

        // 1 tampon yeterli -- icerik hic okunmuyor, sadece varligi/zamanlamasi onemli.
        m_probeFramePool = winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::CreateFreeThreaded(
            m_probeWinrtDevice,
            winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
            1,
            m_probeItem.Size());

        m_probeFrameArrivedRevoker = m_probeFramePool.FrameArrived(
            winrt::auto_revoke,
            [this](auto& pool, auto& /*args*/)
            {
                if (!m_probeActive) return;
                // TryGetNextFrame istisna firlatabilir (bkz. AcquireCurrentFrameSRV'deki ayni
                // konudaki not) -- burada bir WinRT delegate ABI sinirinin icinden cagriliyor,
                // sarilmazsa daha da tehlikeli (yakalanmayan istisna native<->WinRT sinirinda
                // process'i fail-fast ile sonlandirabilir). Probe SADECE zamanlama icin var,
                // basarisiz olursa ana yakalamayi (m_sessionBroken) etkilemeden kendini kapatiyoruz.
                try
                {
                    if (auto frame = pool.TryGetNextFrame())
                    {
                        RecordFrameArrival();
                        frame.Close();
                    }
                }
                catch (...)
                {
                    DLSS_Log("[Capture] FPS probe FrameArrived istisnasi -- probe kapatiliyor "
                             "(ana yakalama etkilenmez).");
                    m_probeActive = false;
                }
            });

        m_probeSession = m_probeFramePool.CreateCaptureSession(m_probeItem);
        m_probeSession.IsCursorCaptureEnabled(false);
        try { m_probeSession.IsBorderRequired(false); } catch (...) {}
        try { m_probeSession.MinUpdateInterval(winrt::Windows::Foundation::TimeSpan{ 0 }); } catch (...) {}
        m_probeSession.StartCapture();

        m_probeActive = true;
        DLSS_Log("[Capture] FPS probe (WGC) basladi (hedef=%s, boyut=%dx%d) -- IN/OUT gostergesi "
                 "artik bu oturumdan besleniyor.", targetHwnd ? "pencere" : "monitor",
                 (int)m_probeItem.Size().Width, (int)m_probeItem.Size().Height);
        return true;
    }
    catch (...)
    {
        DLSS_Log("[Capture] FPS probe (WGC) baslatilamadi -- IN/OUT gostergesi DXGI'nin "
                 "kendi (kendi presentimizle kirlenmis) zamanlamasina donecek");
        StopFpsProbe();
        return false;
    }
}

void CaptureManager::StopFpsProbe()
{
    m_probeActive = false;

    try { m_probeFrameArrivedRevoker.revoke(); } catch (...) {}

    try
    {
        if (m_probeSession) { m_probeSession.Close(); m_probeSession = nullptr; }
    }
    catch (...) {}

    try
    {
        if (m_probeFramePool) { m_probeFramePool.Close(); m_probeFramePool = nullptr; }
    }
    catch (...) {}

    m_probeItem        = nullptr;
    m_probeWinrtDevice = nullptr;
}

bool CaptureManager::QueryWindowContentSize(HWND targetHwnd, int& outWidth, int& outHeight)
{
    if (!targetHwnd) return false;

    auto factory = winrt::get_activation_factory<
        winrt::Windows::Graphics::Capture::GraphicsCaptureItem,
        IGraphicsCaptureItemInterop>();

    winrt::Windows::Graphics::Capture::GraphicsCaptureItem item{ nullptr };
    HRESULT hr = factory->CreateForWindow(
        targetHwnd,
        winrt::guid_of<winrt::Windows::Graphics::Capture::GraphicsCaptureItem>(),
        winrt::put_abi(item));

    if (FAILED(hr) || !item) return false;

    auto size = item.Size();
    if (size.Width <= 0 || size.Height <= 0) return false;

    outWidth  = size.Width;
    outHeight = size.Height;
    return true;
}

bool CaptureManager::StartWithItem(winrt::Windows::Graphics::Capture::GraphicsCaptureItem item, ID3D11Device* device)
{
    m_device = device;

    // ONEMLI: ThrowIfFailed, .as<>(), CreateFreeThreaded, CreateCaptureSession
    // ve StartCapture() hepsi winrt::hresult_error FIRLATABILIR -- ozellikle
    // hedef pencere tam o anda bir focus/aktivasyon gecisi yasiyorsa. Bunlar
    // eskiden hicbir try/catch'e sarili degildi; bir throw dogrudan
    // App::StartOverlayCommon'a kadar cikip TUM UYGULAMAYI cokertiyordu
    // (crash dump kaniti: winrt::hresult_error, BASLAT'a hedef pencereyi
    // odakladiktan hemen sonra basildiginda). Simdi tum govdeyi sarip,
    // basarisizlikta olusabilecek YARIM kurulmus durumu (framePool/session/
    // item/frameEvent) temizleyip false donuyoruz -- caginin zaten bekledigi
    // "capture baslatilamadi" yolu.
    try
    {
        // --- 1. Wrap the D3D11 device as a WinRT IDirect3DDevice ---
        ComPtr<IDXGIDevice> dxgiDevice;
        ThrowIfFailed(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice)),
            "QueryInterface IDXGIDevice");

        // CreateDirect3D11DeviceFromDXGIDevice returns a WinRT object; use
        // winrt::com_ptr (not WRL ComPtr) so we can call .as<>() on it.
        winrt::com_ptr<IInspectable> inspectable;
        ThrowIfFailed(
            CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.Get(), inspectable.put()),
            "CreateDirect3D11DeviceFromDXGIDevice");

        m_winrtDevice = inspectable.as<
            winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();

        m_item        = item;
        m_captureSize = m_item.Size();
        m_width       = m_captureSize.Width;
        m_height      = m_captureSize.Height;

        // WGC oturumu kendi icinden kapanirsa (pencere yok edildi, GPU adaptoru/DPI degisti,
        // WGC'nin kendi ic nedenleri) bu olay ates alir -- bkz. IsSessionBroken() aciklamasi
        // (header). Bunu dinlemezsek m_active/m_newFrame hicbir zaman "olduk" demiyor, App::Run
        // sonsuza kadar yeni kare bekler, overlay sessizce donmus kalir.
        m_itemClosedRevoker = m_item.Closed(
            winrt::auto_revoke,
            [this](auto&, auto&)
            {
                DLSS_Log("[Capture] WGC GraphicsCaptureItem kapandi (hedef kapatilmis/tasinmis/"
                         "WGC ic nedeni olabilir) -- yakalama yeniden baslatilacak.");
                m_sessionBroken.store(true, std::memory_order_release);
            });

        // Create auto-reset event for waking up render thread without CPU spinning
        m_frameEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

        // --- 3. Create free-threaded frame pool (3 back buffers to absorb GPU contention spikes) ---
        m_framePool =
            winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::CreateFreeThreaded(
                m_winrtDevice,
                winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                kFramePoolBufferCount,
                m_captureSize);

        // --- 4. Register FrameArrived callback (fires on any WGC thread) ---
        // bkz. header'daki m_frameCallbackMutex aciklamasi: Stop() bu AYNI kilidi alip
        // m_frameEvent/m_session/m_framePool'u yok etmeden once, o an calismakta olabilecek bir
        // callback'in bitmesini BEKLER -- eskiden Stop()'taki Sleep(15) sezgiseli buna
        // olasiliksal olarak guveniyordu.
        m_frameArrivedRevoker = m_framePool.FrameArrived(
            winrt::auto_revoke,
            [this](auto& /*pool*/, auto& /*args*/)
            {
                std::lock_guard<std::mutex> lock(m_frameCallbackMutex);
                if (!m_active) return;
                m_newFrame.store(true, std::memory_order_release);
                HANDLE evt = m_frameEvent;
                if (evt)
                    SetEvent(evt);

                RecordFrameArrival();
            });

        // --- 5. Create session and start capture ---
        m_session = m_framePool.CreateCaptureSession(m_item);
        m_session.IsCursorCaptureEnabled(false); // hide WGC cursor overlay

        // Windows 10/11: Disable capture border and enable unthrottled capture updates
        try
        {
            m_session.IsBorderRequired(false);
        }
        catch (...) {}

        try
        {
            // Set minimum update interval to 0 (unthrottled / maximum game frame rate)
            m_session.MinUpdateInterval(winrt::Windows::Foundation::TimeSpan{ 0 });
        }
        catch (...) {}

        m_session.StartCapture();

        m_active = true;
        m_sessionBroken.store(false, std::memory_order_release);
        return true;
    }
    catch (const winrt::hresult_error& ex)
    {
        DLSS_Log("[Capture] StartWithItem winrt istisnasi: 0x%08X (%ls) -- yarim kurulum temizleniyor.",
            (unsigned)ex.code(), ex.message().c_str());
    }
    catch (...)
    {
        DLSS_Log("[Capture] StartWithItem bilinmeyen istisna -- yarim kurulum temizleniyor.");
    }

    // Yarim kalmis olabilecek durumu temizle (Stop() burada guvenle
    // kullanilamaz: m_active hala false, ustteki guard'i tetikler ve hicbir
    // sey yapmadan doner).
    try { m_frameArrivedRevoker.revoke(); } catch (...) {}
    try { m_itemClosedRevoker.revoke();   } catch (...) {}
    try { if (m_session)   { m_session.Close();   m_session   = nullptr; } } catch (...) {}
    try { if (m_framePool) { m_framePool.Close(); m_framePool = nullptr; } } catch (...) {}
    if (m_frameEvent) { CloseHandle(m_frameEvent); m_frameEvent = nullptr; }
    m_item        = nullptr;
    m_winrtDevice = nullptr;
    return false;
}

// -----------------------------------------------------------------------
// RecordFrameArrival — shared frame-gap/input-FPS bookkeeping.
// Called once per genuinely new frame by both the WGC FrameArrived callback and the
// DXGI Desktop Duplication poll path.
// -----------------------------------------------------------------------
void CaptureManager::RecordFrameArrival()
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (m_lastFrameArrivalTime.QuadPart != 0)
    {
        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);
        double gapMs = static_cast<double>(now.QuadPart - m_lastFrameArrivalTime.QuadPart)
                       * 1000.0 / static_cast<double>(freq.QuadPart);

        m_lastFrameGapMs = gapMs;
        if (gapMs > m_maxFrameGapMs) m_maxFrameGapMs = gapMs;
        m_sumFrameGapMs += gapMs;
        m_frameGapSamples++;

        m_gapHistory[m_gapHistoryIdx % kJitterWindow] = gapMs;
        m_gapHistoryIdx++;

        // Threshold: >200 ms means capture may have been paused (window minimised/occluded)
        if (gapMs > 200.0)
            DLSS_Log("[Capture] UYARI: iki kare arasinda %.1f ms bosluk "
                     "(pencere minimize/occluded olmus ya da capture session durmus olabilir)", gapMs);

        // FPS Tracking
        m_inputFpsFrameCount++;
        if (m_inputFpsLastTime.QuadPart == 0)
        {
            m_inputFpsLastTime = now;
        }
        else
        {
            double elapsed = static_cast<double>(now.QuadPart - m_inputFpsLastTime.QuadPart) / static_cast<double>(freq.QuadPart);
            if (elapsed >= 0.5)
            {
                m_currentInputFps = static_cast<int>((m_inputFpsFrameCount / elapsed) + 0.5);
                m_inputFpsFrameCount = 0;
                m_inputFpsLastTime = now;
            }
        }
    }
    else
    {
        m_inputFpsLastTime = now;
    }
    m_lastFrameArrivalTime = now;
}

// -----------------------------------------------------------------------
// Stop
// -----------------------------------------------------------------------
void CaptureManager::Stop()
{
    if (!m_active) return;
    m_active = false;

    try
    {
        m_frameArrivedRevoker.revoke();
    }
    catch (...) {}

    try
    {
        m_itemClosedRevoker.revoke();
    }
    catch (...) {}

    // Eskiden burada sadece "Sleep(15) callback'i bitirmistir" varsayimi vardi -- revoke()
    // SADECE gelecekteki cagrilari engeller, o an WGC'nin kendi thread pool'unda calismakta
    // olan bir FrameArrived cagrisini beklemez. FrameArrived govdesi m_frameCallbackMutex'i
    // tutarak calisiyor (bkz. StartWithItem); burada ayni kilidi alip hemen birakmak, asagida
    // m_frameEvent'i kapatip m_session/m_framePool'u yok etmeden once o an calismakta olabilecek
    // bir cagrinin kesinlikle bittiginden emin olur -- olasiliksal degil, deterministik.
    {
        std::lock_guard<std::mutex> lock(m_frameCallbackMutex);
    }

    ReleaseCurrentFrame();

    if (m_nextFrame)
    {
        m_nextFrame.Close();
        m_nextFrame = nullptr;
    }

    for (auto& entry : m_srvCache)
    {
        entry.texture = nullptr;
        entry.srv.Reset();
    }

    try
    {
        if (m_session)
        {
            m_session.Close();
            m_session = nullptr;
        }
    }
    catch (...) {}

    try
    {
        if (m_framePool)
        {
            m_framePool.Close();
            m_framePool = nullptr;
        }
    }
    catch (...) {}

    if (m_frameEvent)
    {
        CloseHandle(m_frameEvent);
        m_frameEvent = nullptr;
    }

    TeardownDuplication();

    m_item        = nullptr;
    m_winrtDevice = nullptr;
    m_device      = nullptr;
    m_backend     = CaptureBackend::WGC;

    m_newFrame.store(false, std::memory_order_relaxed);
    m_resized .store(false, std::memory_order_relaxed);
    m_sessionBroken.store(false, std::memory_order_relaxed);
}

// -----------------------------------------------------------------------
// AcquireCurrentFrameSRV — Zero-Copy: binds WGC surface directly to shader
// (dispatches to the DXGI Desktop Duplication path when that backend is active)
// -----------------------------------------------------------------------
ID3D11ShaderResourceView* CaptureManager::AcquireCurrentFrameSRV(ID3D11Device* device)
{
    if (m_backend == CaptureBackend::DXGIDuplication)
    {
        // Artik AcquireNextFrame'i BURADAN (render thread) cagirmiyoruz -- bkz.
        // DupPollThreadProc/header. Kendi thread'inde uretilmis en son sonucu okuyup
        // tuketiyoruz; yakalama hizi bu thread'in ne kadar sik ugradigina baglı degil.
        if (!m_newFrame.load(std::memory_order_acquire)) return nullptr;
        m_newFrame.store(false, std::memory_order_relaxed);
        return m_dupLatestSRV.load(std::memory_order_acquire);
    }

    if (!m_newFrame.load(std::memory_order_acquire)) return nullptr;

    // ONEMLI: TryGetNextFrame/ContentSize/Recreate/Surface/.as<> hepsi C++/WinRT projeksiyonlari
    // olup basarisizlikta HRESULT DEGIL, winrt::hresult_error FIRLATIR. WGC oturumu kare
    // yakalama SIRASINDA gecersiz hale gelirse (pencere yeniden olusturuldu, GPU adaptoru/DPI
    // degisti, ya da GraphicsCaptureItem WGC'nin kendi ic nedeniyle kapandi) bu cagrilardan
    // biri istisna firlatir. Start()/StartWithItem() bunu baslangicta zaten try/catch'e almisti
    // (bkz. oradaki not) ama HER KAREDE calisan bu fonksiyon sarilmamisti -- App::Run() de
    // dogrudan cagirdigindan, yakalanmayan bir istisna App::Run()'a kadar cikip TUM UYGULAMAYI
    // cokertiyordu. Simdi tum govdeyi sarip, hata durumunda oturumu "bozuk" isaretliyoruz
    // (IsSessionBroken()) ki App bunu tespit edip yakalamayi guvenli sekilde yeniden baslatsin.
    try
    {
        winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame frame{ nullptr };

        if (m_nextFrame)
        {
            frame = m_nextFrame;
            m_nextFrame = nullptr;
        }
        else
        {
            frame = m_framePool.TryGetNextFrame();
        }

        if (!frame)
        {
            m_newFrame.store(false, std::memory_order_relaxed);
            m_nullFrameStreak++;
            if (m_nullFrameStreak == kNullFrameWarnThreshold)
                DLSS_Log("[Capture] UYARI: %u ardisik TryGetNextFrame() null donus "
                         "(WGC session donmus ya da oyun penceresi gizlenmis olabilir)", kNullFrameWarnThreshold);
            return nullptr;
        }
        m_nullFrameStreak = 0; // sifirla: gecerli frame geldi

        // Check if another frame has already arrived in the pool
        auto peekFrame = m_framePool.TryGetNextFrame();
        if (peekFrame)
        {
            // Balanced frame pacing:
            // If 3 or more frames accumulated (e.g. after a loading hitch),
            // discard the oldest frame while preserving sequential cadence for DLSS temporal stability.
            auto thirdFrame = m_framePool.TryGetNextFrame();
            if (thirdFrame)
            {
                frame.Close();
                frame = peekFrame;
                m_nextFrame = thirdFrame;

                // Drain any extreme backlog (4+ frames)
                while (auto extra = m_framePool.TryGetNextFrame())
                {
                    m_nextFrame.Close();
                    m_nextFrame = extra;
                }
            }
            else
            {
                m_nextFrame = peekFrame;
            }

            // Keep m_newFrame = true so the render loop immediately consumes the next queued frame without sleeping
            m_newFrame.store(true, std::memory_order_release);
        }
        else
        {
            m_nextFrame = nullptr;
            m_newFrame.store(false, std::memory_order_relaxed);
        }

        // --- Check for window resize ---
        auto size = frame.ContentSize();
        if (size.Width != m_captureSize.Width || size.Height != m_captureSize.Height)
        {
            if (m_nextFrame)
            {
                m_nextFrame.Close();
                m_nextFrame = nullptr;
            }

            m_captureSize = size;
            m_width       = size.Width;
            m_height      = size.Height;
            m_resized.store(true, std::memory_order_relaxed);

            // Clear cached SRVs on resize
            for (auto& entry : m_srvCache)
            {
                entry.texture = nullptr;
                entry.srv.Reset();
            }

            m_framePool.Recreate(
                m_winrtDevice,
                winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                kFramePoolBufferCount,
                size);

            frame.Close();
            m_newFrame.store(false, std::memory_order_relaxed);
            return nullptr;
        }

        // --- Extract D3D11 texture from WGC surface ---
        auto surface    = frame.Surface();
        auto dxgiAccess = surface.as<
            ::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();

        ComPtr<ID3D11Texture2D> srcTexture;
        HRESULT hr = dxgiAccess->GetInterface(IID_PPV_ARGS(&srcTexture));
        if (FAILED(hr) || !srcTexture)
        {
            frame.Close();
            return nullptr;
        }

        // --- Look up or populate cached SRV for this pool buffer ---
        ID3D11ShaderResourceView* resultSRV = nullptr;
        for (auto& entry : m_srvCache)
        {
            if (entry.texture == srcTexture.Get())
            {
                resultSRV = entry.srv.Get();
                break;
            }
        }

        if (!resultSRV)
        {
            size_t slot = 0;
            for (size_t i = 0; i < kMaxCachedSRVs; ++i)
            {
                if (!m_srvCache[i].texture)
                {
                    slot = i;
                    break;
                }
            }

            D3D11_SHADER_RESOURCE_VIEW_DESC srvd = {};
            srvd.Format                    = DXGI_FORMAT_B8G8R8A8_UNORM;
            srvd.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
            srvd.Texture2D.MipLevels       = 1;
            srvd.Texture2D.MostDetailedMip = 0;

            ComPtr<ID3D11ShaderResourceView> newSRV;
            hr = device->CreateShaderResourceView(srcTexture.Get(), &srvd, &newSRV);
            if (SUCCEEDED(hr))
            {
                m_srvCache[slot].texture = srcTexture.Get();
                m_srvCache[slot].srv     = newSRV;
                resultSRV                = newSRV.Get();
            }
        }

        if (resultSRV)
        {
            // Hold frame locked until ReleaseCurrentFrame() is called after Present()
            ReleaseCurrentFrame();
            m_currentFrame = frame;
            return resultSRV;
        }

        frame.Close();
        return nullptr;
    }
    catch (const winrt::hresult_error& ex)
    {
        DLSS_Log("[Capture] AcquireCurrentFrameSRV winrt istisnasi: 0x%08X (%ls) -- WGC oturumu "
                 "bozuk sayiliyor, yeniden baslatma tetiklenecek.",
                 (unsigned)ex.code(), ex.message().c_str());
        m_newFrame.store(false, std::memory_order_relaxed);
        m_sessionBroken.store(true, std::memory_order_release);
        return nullptr;
    }
    catch (...)
    {
        DLSS_Log("[Capture] AcquireCurrentFrameSRV bilinmeyen istisna -- WGC oturumu bozuk "
                 "sayiliyor, yeniden baslatma tetiklenecek.");
        m_newFrame.store(false, std::memory_order_relaxed);
        m_sessionBroken.store(true, std::memory_order_release);
        return nullptr;
    }
}

// -----------------------------------------------------------------------
// ReleaseCurrentFrame — call after Present to return surface to frame pool
// -----------------------------------------------------------------------
void CaptureManager::ReleaseCurrentFrame()
{
    if (m_currentFrame)
    {
        m_currentFrame.Close();
        m_currentFrame = nullptr;
    }
}

// -----------------------------------------------------------------------
// CopyFrame — legacy fallback
// -----------------------------------------------------------------------
bool CaptureManager::CopyFrame(ID3D11DeviceContext* ctx, ID3D11Texture2D* dst)
{
    if (!m_newFrame.load(std::memory_order_acquire)) return false;

    // bkz. AcquireCurrentFrameSRV'deki ayni konudaki not: TryGetNextFrame/ContentSize/Recreate/
    // Surface/.as<> istisna firlatabilir, sarilmazsa cagiranin (App::Run) tepesine kadar cikip
    // uygulamayi cokertir.
    try
    {
        auto frame = m_framePool.TryGetNextFrame();
        if (!frame) return false;

        // Latency eliminator: if multiple frames arrived while rendering/processing,
        // drain the queue and keep the absolute freshest frame!
        while (auto newerFrame = m_framePool.TryGetNextFrame())
        {
            frame.Close();
            frame = newerFrame;
        }

        m_newFrame.store(false, std::memory_order_relaxed);

        // --- Check for window resize ---
        auto size = frame.ContentSize();
        if (size.Width != m_captureSize.Width || size.Height != m_captureSize.Height)
        {
            m_captureSize = size;
            m_width       = size.Width;
            m_height      = size.Height;
            m_resized.store(true, std::memory_order_relaxed);

            m_framePool.Recreate(
                m_winrtDevice,
                winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                kFramePoolBufferCount,
                size);

            frame.Close();
            return false;
        }

        // --- Extract the D3D11 texture from the WGC frame surface ---
        auto surface     = frame.Surface();
        auto dxgiAccess  = surface.as<
            ::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();

        ComPtr<ID3D11Texture2D> srcTexture;
        HRESULT hr = dxgiAccess->GetInterface(IID_PPV_ARGS(&srcTexture));

        if (SUCCEEDED(hr) && srcTexture && dst)
        {
            ctx->CopyResource(dst, srcTexture.Get());
        }

        frame.Close();
        return SUCCEEDED(hr);
    }
    catch (const winrt::hresult_error& ex)
    {
        DLSS_Log("[Capture] CopyFrame winrt istisnasi: 0x%08X (%ls) -- WGC oturumu bozuk sayiliyor.",
            (unsigned)ex.code(), ex.message().c_str());
        m_newFrame.store(false, std::memory_order_relaxed);
        m_sessionBroken.store(true, std::memory_order_release);
        return false;
    }
    catch (...)
    {
        DLSS_Log("[Capture] CopyFrame bilinmeyen istisna -- WGC oturumu bozuk sayiliyor.");
        m_newFrame.store(false, std::memory_order_relaxed);
        m_sessionBroken.store(true, std::memory_order_release);
        return false;
    }
}

// -----------------------------------------------------------------------
// FlushCaptureStats — call every ~5 s from the render thread
//   Logs a health summary of WGC frame delivery and resets accumulators.
// -----------------------------------------------------------------------
void CaptureManager::FlushCaptureStats()
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    // Throttle: only log every 5 seconds
    if (m_lastGapLogTime.QuadPart != 0)
    {
        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);
        double sinceLastMs = static_cast<double>(now.QuadPart - m_lastGapLogTime.QuadPart)
                             * 1000.0 / static_cast<double>(freq.QuadPart);
        if (sinceLastMs < 5000.0) return;
    }
    m_lastGapLogTime = now;

    if (m_frameGapSamples > 0)
    {
        double mean = m_sumFrameGapMs / static_cast<double>(m_frameGapSamples);
        int windowCount = std::min(kJitterWindow, m_gapHistoryIdx);
        double variance = 0.0;
        if (windowCount > 0)
        {
            for (int i = 0; i < windowCount; ++i)
            {
                double diff = m_gapHistory[i] - mean;
                variance += diff * diff;
            }
            variance /= windowCount;
        }
        double stddev = std::sqrt(variance);

        DLSS_Log("[Capture] ozet(5sn): ort_gap=%.1fms stddev=%.1fms maks=%.1fms | "
                 "currentInputFps=%d probe_aktif=%d backend=%s "
                 "(stddev/ort orani yuksekse = duzensiz gelis, DWM/FG pacing supheli)",
                 mean, stddev, m_maxFrameGapMs, m_currentInputFps, m_probeActive ? 1 : 0,
                 (m_backend == CaptureBackend::DXGIDuplication) ? "DXGI" : "WGC");

        // Reset accumulators for the next window
        m_maxFrameGapMs   = 0.0;
        m_sumFrameGapMs   = 0.0;
        m_frameGapSamples = 0;
        m_gapHistoryIdx   = 0;
    }
}
