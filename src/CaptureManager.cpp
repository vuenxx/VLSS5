#include "CaptureManager.h"

// 3 frame pool buffers allow GPU to pipelining without dropping frames
static constexpr int32_t kFramePoolBufferCount = 3;

// -----------------------------------------------------------------------
// Start
// -----------------------------------------------------------------------
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

    auto frame = m_framePool.TryGetNextFrame();
    if (!frame) return nullptr;

    // Latency eliminator: if multiple frames arrived, keep the absolute freshest
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
            3,
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
