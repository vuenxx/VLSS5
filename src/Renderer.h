#pragma once
#include "Common.h"
#include "MotionVectorManager.h"
#include "DLSSManager.h"
#include "D3D12Interop.h"
#include "DLSSNRManager.h"

// ---------------------------------------------------------------------------
// Renderer
//   D3D11 swap chain + full-screen-triangle shader that blits the WGC
//   captured texture directly to the overlay window.
// ---------------------------------------------------------------------------
class Renderer
{
public:
    Renderer()  = default;
    ~Renderer() { Cleanup(); }

    // Create swap chain for the overlay HWND and compile the two shaders.
    bool Init(ID3D11Device* device, HWND overlayHwnd, int width, int height);

    // Resize swap chain buffers when the captured window changes size.
    void Resize(ID3D11Device* device, int width, int height);

    // Create (or recreate) the D3D11 texture that receives WGC frames.
    // The texture format matches WGC output: DXGI_FORMAT_B8G8R8A8_UNORM.
    bool CreateCaptureTexture(ID3D11Device* device, int width, int height);

    // Draw the capture texture as a full-screen triangle.
    void RenderFrame(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* srv);

    // Present the back buffer.
    void Present();

    // VSync controls
    void SetVSyncEnabled(bool enabled) { m_vsyncEnabled = enabled; }
    bool IsVSyncEnabled() const { return m_vsyncEnabled; }

    // Release all D3D resources.
    void Cleanup();

    // FPS counter controls & updates
    void UpdateFps(ID3D11DeviceContext* ctx, int fps, bool forceRedraw = false);
    void SetFpsEnabled(bool enabled) { m_fpsEnabled = enabled; }
    bool IsFpsEnabled() const { return m_fpsEnabled; }
    void UpdateFpsConstantBuffer(ID3D11DeviceContext* ctx);

    void  SetBoostFactor(float v) { if (fabs(m_boostFactor - v) > 0.001f) { m_boostFactor = v; m_cbufferDirty = true; } }
    float GetBoostFactor() const  { return m_boostFactor; }

    void  SetSplitScreen(bool enabled, float pos)
    {
        if (m_splitEnabled != enabled || fabs(m_splitPos - pos) > 0.001f)
        {
            m_splitEnabled = enabled;
            m_splitPos     = pos;
            m_cbufferDirty = true;
        }
    }
    bool  IsSplitScreenEnabled() const { return m_splitEnabled; }
    float GetSplitPos() const          { return m_splitPos; }

    // Accessors used by App.
    ID3D11Texture2D*          GetCaptureTexture() const { return m_captureTexture.Get(); }
    ID3D11ShaderResourceView* GetCaptureSRV()     const { return m_captureSRV.Get();     }
    int                       GetWidth()          const { return m_width;  }
    int                       GetHeight()         const { return m_height; }
    double                    GetLastMvMs()       const { return m_lastMvMs;   }
    double                    GetLastEvalMs()     const { return m_lastEvalMs; }

    DLSSManager*              GetDLSSManager()         { return m_dlssManager.get();         }
    DLSSNRManager*            GetDLSSNRManager()       { return m_dlssnrManager.get();       }
    D3D12Interop*             GetD3D12Interop()        { return m_d3d12Interop.get();        }
    MotionVectorManager*      GetMotionVectorManager() { return m_motionVectorManager.get(); }

private:
    bool CompileShaders(ID3D11Device* device);
    bool CreateSwapChain(ID3D11Device* device, HWND hwnd, int width, int height);
    bool CreateRTV(ID3D11Device* device);
    bool CreateFpsResources(ID3D11Device* device);

    ComPtr<IDXGISwapChain1>          m_swapChain;
    ComPtr<ID3D11RenderTargetView>   m_rtv;
    ComPtr<ID3D11VertexShader>       m_vs;
    ComPtr<ID3D11PixelShader>        m_ps;
    ComPtr<ID3D11SamplerState>       m_sampler;
    ComPtr<ID3D11SamplerState>       m_linearSampler;
    ComPtr<ID3D11Texture2D>          m_captureTexture;
    ComPtr<ID3D11ShaderResourceView> m_captureSRV;

    // FPS display resources
    ComPtr<ID3D11Texture2D>          m_fpsTexture;
    ComPtr<ID3D11ShaderResourceView> m_fpsSRV;
    ComPtr<ID3D11Buffer>             m_fpsCBuffer;
    HDC                              m_hFpsDC          = nullptr;
    HBITMAP                          m_hFpsBmp         = nullptr;
    void*                            m_pFpsBits        = nullptr;
    bool                             m_fpsEnabled      = true;
    int                              m_lastRenderedFps = -1;
    bool                             m_lastRenderedDlss = false;
    bool                             m_lastRenderedDlssNr = false;

    int   m_width            = 0;
    int   m_height           = 0;
    bool  m_tearingSupported = false;
    bool  m_vsyncEnabled     = false;
    bool  m_dlssnrActive     = false;
    bool  m_isSubNative      = false;
    float m_intensity        = 1.0f;
    float m_boostFactor      = 1.0f;
    bool  m_splitEnabled     = false;
    float m_splitPos         = 0.5f;
    float m_colourStrength   = 1.0f;
    float m_workTexelSize[2] = { 0.0f, 0.0f };
    bool  m_cbufferDirty     = true;
    double m_lastMvMs        = 0.0;
    double m_lastEvalMs      = 0.0;

    std::unique_ptr<DLSSManager>         m_dlssManager;
    std::unique_ptr<DLSSNRManager>       m_dlssnrManager;
    std::unique_ptr<D3D12Interop>        m_d3d12Interop;
    std::unique_ptr<MotionVectorManager> m_motionVectorManager;
};
