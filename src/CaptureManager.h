#pragma once
#include "Common.h"

// ---------------------------------------------------------------------------
// CaptureManager
//   Windows Graphics Capture (WGC) — captures a specific HWND into a
//   D3D11 texture using a free-threaded frame pool.
// ---------------------------------------------------------------------------
class CaptureManager
{
public:
    CaptureManager()  = default;
    ~CaptureManager() { Stop(); }

    // Begin capturing the target HWND.
    // Returns false if WGC is unsupported or the HWND is invalid.
    bool Start(HWND targetHwnd, ID3D11Device* device);

    // Stop capturing and release all WGC resources.
    void Stop();

    bool IsActive()              const { return m_active; }
    bool IsNewFrameAvailable()   const { return m_newFrame.load(std::memory_order_relaxed); }
    HANDLE GetFrameEvent()       const { return m_frameEvent; }

    int  GetWidth()              const { return m_width;  }
    int  GetHeight()             const { return m_height; }

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
    struct SRVCacheEntry
    {
        ID3D11Texture2D* texture = nullptr;
        ComPtr<ID3D11ShaderResourceView> srv;
    };
    static constexpr size_t kMaxCachedSRVs = 4;
    SRVCacheEntry m_srvCache[kMaxCachedSRVs];

    winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame m_currentFrame{ nullptr };

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
    ComPtr<ID3D11Device> m_device;

    std::atomic<bool> m_newFrame{ false };
    std::atomic<bool> m_resized { false };
};
