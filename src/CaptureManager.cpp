#include "CaptureManager.h"

// 5 frame pool buffers allow GPU pipelining at high refresh rates (120-240 FPS / Frame Gen) without dropping frames
static constexpr int32_t kFramePoolBufferCount = 5;

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

bool CaptureManager::Start(HWND targetHwnd, ID3D11Device* device)
{
    Stop(); // clean up any prior session

    m_device = device;

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

    // --- 2. Create GraphicsCaptureItem for the target HWND ---
    auto factory = winrt::get_activation_factory<
        winrt::Windows::Graphics::Capture::GraphicsCaptureItem,
        IGraphicsCaptureItemInterop>();

    winrt::Windows::Graphics::Capture::GraphicsCaptureItem item{ nullptr };
    HRESULT hr = factory->CreateForWindow(
        targetHwnd,
        winrt::guid_of<winrt::Windows::Graphics::Capture::GraphicsCaptureItem>(),
        winrt::put_abi(item));

    if (FAILED(hr) || !item) return false;

    m_item        = item;
    m_captureSize = m_item.Size();
    m_width       = m_captureSize.Width;
    m_height      = m_captureSize.Height;

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
    m_frameArrivedRevoker = m_framePool.FrameArrived(
        winrt::auto_revoke,
        [this](auto& /*pool*/, auto& /*args*/)
        {
            if (!m_active) return;
            m_newFrame.store(true, std::memory_order_release);
            HANDLE evt = m_frameEvent;
            if (evt)
                SetEvent(evt);

            // --- WGC frame-gap health measurement ---
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

                // Threshold: >200 ms means WGC may have been paused (window minimised/occluded)
                if (gapMs > 200.0)
                    DLSS_Log("[Capture] UYARI: iki WGC karesi arasinda %.1f ms bosluk "
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
    return true;
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

    // Brief sleep to let any in-flight background callback finish cleanly
    Sleep(15);

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

    m_item        = nullptr;
    m_winrtDevice = nullptr;
    m_device      = nullptr;

    m_newFrame.store(false, std::memory_order_relaxed);
    m_resized .store(false, std::memory_order_relaxed);
}

// -----------------------------------------------------------------------
// AcquireCurrentFrameSRV — Zero-Copy: binds WGC surface directly to shader
// -----------------------------------------------------------------------
ID3D11ShaderResourceView* CaptureManager::AcquireCurrentFrameSRV(ID3D11Device* device)
{
    if (!m_newFrame.load(std::memory_order_acquire)) return nullptr;

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

        DLSS_Log("[Capture] ozet(5sn): ort_gap=%.1fms stddev=%.1fms maks=%.1fms "
                 "(stddev/ort orani yuksekse = duzensiz gelis, DWM/FG pacing supheli)",
                 mean, stddev, m_maxFrameGapMs);

        // Reset accumulators for the next window
        m_maxFrameGapMs   = 0.0;
        m_sumFrameGapMs   = 0.0;
        m_frameGapSamples = 0;
        m_gapHistoryIdx   = 0;
    }
}
