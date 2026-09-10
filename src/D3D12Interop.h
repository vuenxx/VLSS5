#pragma once
#include "Common.h"
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>

class MotionVectorManager;

class D3D12Interop
{
public:
    D3D12Interop() = default;
    ~D3D12Interop() { Cleanup(); }

    bool Init(ID3D11Device* d3d11Dev, ID3D11DeviceContext* d3d11Ctx, int width, int height, int workWidth = 0, int workHeight = 0);
    void Cleanup();
    bool Resize(int width, int height, int workWidth = 0, int workHeight = 0);
    bool ResizeWork(int workWidth, int workHeight);

    // Synchronize D3D11 capture -> D3D12 input (Zero-Copy VRAM copy or bilinear downscale + Optical Flow + GPU fence)
    bool BeginFrame(
        ID3D11Texture2D* srcCapturedTex,
        ID3D11ShaderResourceView* srcCapturedSRV = nullptr,
        MotionVectorManager* mvMgr = nullptr);

    // Finish D3D12 execution and synchronize back to D3D11
    bool EndFrame();

    ID3D12Device*               GetDevice()       const { return m_d3d12Device.Get(); }
    ID3D12CommandQueue*         GetCommandQueue() const { return m_cmdQueue.Get();    }
    ID3D12GraphicsCommandList*  GetCommandList()  const { return m_cmdList.Get();     }

    ID3D12Resource*             GetInputD12()     const { return m_sharedInD12.Get();  }
    ID3D12Resource*             GetOutputD12()    const { return m_nativeOutD12.Get(); }
    ID3D12Resource*             GetMotionD12()    const { return m_sharedMvD12.Get();  }

    ID3D11Texture2D*            GetInputD11()     const { return m_sharedInD11.Get();  }
    ID3D11ShaderResourceView*   GetInputSRV()     const { return m_sharedInSRV.Get();  }
    ID3D11Texture2D*            GetOutputD11()    const { return m_sharedOutD11.Get(); }
    ID3D11ShaderResourceView*   GetOutputSRV()    const { return m_sharedOutSRV.Get(); }
    ID3D11Texture2D*            GetMotionD11()    const { return m_sharedMvD11.Get();  }
    ID3D11UnorderedAccessView*  GetMotionUAV()    const { return m_sharedMvUAV.Get();  }
    ID3D11ShaderResourceView*   GetMotionSRV()    const { return m_sharedMvSRV.Get();  }

    int GetWidth()      const { return m_width;  }
    int GetHeight()     const { return m_height; }
    int GetWorkWidth()  const { return m_workWidth;  }
    int GetWorkHeight() const { return m_workHeight; }

    void WaitForGpu();
    void LogDiagnosticPixels(uint64_t frameCount);

private:
    bool CreateSharedTextures(int workWidth, int workHeight);
    bool CreateDownscaleResources();
    bool CreateFences();

    int m_width      = 0;
    int m_height     = 0;
    int m_workWidth  = 0;
    int m_workHeight = 0;

    // D3D11 references
    ComPtr<ID3D11Device>         m_d3d11Dev;
    ComPtr<ID3D11DeviceContext>  m_d3d11Ctx;
    ComPtr<ID3D11Device5>        m_d3d11Dev5;
    ComPtr<ID3D11DeviceContext4> m_d3d11Ctx4;

    // D3D12 core
    ComPtr<ID3D12Device>               m_d3d12Device;
    ComPtr<ID3D12CommandQueue>         m_cmdQueue;
    static constexpr UINT              kCmdAllocCount = 2;
    ComPtr<ID3D12CommandAllocator>     m_cmdAlloc[kCmdAllocCount];
    UINT                               m_allocIndex = 0;
    ComPtr<ID3D12GraphicsCommandList>  m_cmdList;

    // Shared resources (Input: Captured WGC Frame)
    ComPtr<ID3D11Texture2D>            m_sharedInD11;
    ComPtr<ID3D11RenderTargetView>     m_sharedInRTV;
    ComPtr<ID3D11ShaderResourceView>   m_sharedInSRV;
    ComPtr<ID3D12Resource>             m_sharedInD12;

    // Downscale pipeline resources for scale < 100%
    ComPtr<ID3D11VertexShader>         m_downscaleVS;
    ComPtr<ID3D11PixelShader>          m_downscalePS;
    ComPtr<ID3D11SamplerState>         m_downscaleSampler;

    // Shared resources (Motion Vectors: Optical Flow)
    ComPtr<ID3D11Texture2D>            m_sharedMvD11;
    ComPtr<ID3D11UnorderedAccessView>  m_sharedMvUAV;
    ComPtr<ID3D11ShaderResourceView>   m_sharedMvSRV;
    ComPtr<ID3D12Resource>             m_sharedMvD12;

    // Shared resources (Output: DLSS-NR Processed Frame)
    ComPtr<ID3D11Texture2D>            m_sharedOutD11;
    ComPtr<ID3D11ShaderResourceView>   m_sharedOutSRV;
    ComPtr<ID3D12Resource>             m_sharedOutD12;
    ComPtr<ID3D12Resource>             m_nativeOutD12;

    // Hardware GPU Fences for D3D11 <-> D3D12 sync
    ComPtr<ID3D12Fence> m_fenceInD12;
    ComPtr<ID3D11Fence> m_fenceInD11;

    ComPtr<ID3D12Fence> m_fenceOutD12;
    ComPtr<ID3D11Fence> m_fenceOutD11;

    HANDLE m_hFenceEvent = nullptr;

    // Diagnostic readback staging textures
    ComPtr<ID3D11Texture2D> m_diagStagingIn;
    ComPtr<ID3D11Texture2D> m_diagStagingOut;

    UINT64 m_frameIndex = 0;
};
