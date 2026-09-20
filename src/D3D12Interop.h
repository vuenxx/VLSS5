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
    ID3D12Resource*             GetOutputD12()    const { return m_useDirectSharedOut ? m_sharedOutD12.Get() : m_nativeOutD12.Get(); }
    ID3D12Resource*             GetMotionD12()    const { return m_sharedMvD12.Get();  }

    ID3D11Texture2D*            GetInputD11()     const { return m_sharedInD11.Get();  }
    ID3D11ShaderResourceView*   GetInputSRV()     const { return m_sharedInSRV.Get();  }
    ID3D11ShaderResourceView*   GetRawInputSRV()  const { return m_sharedInSRV.Get();  }
    ID3D11Texture2D*            GetOutputD11()    const { return m_sharedOutD11.Get(); }
    ID3D11ShaderResourceView*   GetOutputSRV()    const { return m_sharedOutSRV.Get(); }
    ID3D11Texture2D*            GetMotionD11()    const { return m_sharedMvD11.Get();  }
    ID3D11UnorderedAccessView*  GetMotionUAV()    const { return m_sharedMvUAV.Get();  }
    ID3D11ShaderResourceView*   GetMotionSRV()    const { return m_sharedMvSRV.Get();  }

    int GetWidth()      const { return m_width;  }
    int GetHeight()     const { return m_height; }
    int GetWorkWidth()  const { return m_workWidth;  }
    int GetWorkHeight() const { return m_workHeight; }

    void SetExplicitFlush(bool enable) { m_enableExplicitFlush = enable; }
    bool IsExplicitFlushEnabled() const { return m_enableExplicitFlush; }

    double GetLastMvMs() const { return m_lastMvMs; }

    // D3D12 kuyrugunda gecen GERCEK GPU suresi (DLSS-NR degerlendirmesi dahil).
    //
    // Bunu ayrica olcmek zorundayiz: App'teki "render" sayaci CPU'nun komut
    // gondermesinin ne kadar surdugudur, GPU'nun o komutlari isletmesinin degil.
    // Her sey asenkron oldugu icin CPU 1 ms'de donebilirken GPU ayni ise 40 ms
    // harcayabilir; o maliyet sonra Present() bloke olarak ortaya cikar ve
    // "boru hatti bedava, suclu DWM" gibi yanlis bir tabloya yol acar.
    double GetLastGpuMs() const { return m_lastGpuMs; }

    void WaitForGpu();
    void LogDiagnosticPixels(uint64_t frameCount);

    // DLSS ayri bir GPU'da mi kosuyor? true ise her kare PCIe uzerinden
    // giris + hareket vektoru + cikis transferi yapiliyor demektir.
    bool IsCrossAdapter() const { return m_crossAdapter; }

    // Yakalama/sunum ve DLSS adapter aciklamalari (durum cubugu / log icin).
    const std::wstring& GetCaptureGpuName() const { return m_captureGpuName; }
    const std::wstring& GetDlssGpuName()    const { return m_dlssGpuName;    }

    // Cross-adapter modda kare basina PCIe uzerinden tasinan bayt sayisi.
    // Tek-adapter modda 0.
    size_t GetBridgeBytesPerFrame() const { return m_bridgeBytesPerFrame; }

private:
    bool CreateSharedTextures(int workWidth, int workHeight);
    bool CreateDownscaleResources();
    bool CreateFences();

    // --- Cross-adapter kopru ---
    // A = yakalama/sunum GPU'su (D3D11 burada), B = DLSS GPU'su (m_d3d12Device).
    bool InitCrossAdapterDevice(IDXGIAdapter* captureAdapter);
    bool CreateCrossAdapterFences();
    bool CreateCrossAdapterBridge(int workWidth, int workHeight);
    void CleanupCrossAdapter();

    // A tarafindaki D3D11 dokusunu A'nin D3D12 cihazinda acar.
    bool OpenOnSrcDevice(ID3D11Texture2D* tex, ComPtr<ID3D12Resource>& out, const char* label);

    // A: D3D11 ciktisi -> cross-adapter tampon. BeginFrame icinden cagrilir.
    bool SubmitUploadToBridge();
    // A: cross-adapter tampon -> D3D11 sunum dokusu. EndFrame icinden cagrilir.
    bool SubmitDownloadFromBridge();

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
    static constexpr UINT              kCmdAllocCount = 3;
    ComPtr<ID3D12CommandAllocator>     m_cmdAlloc[kCmdAllocCount];
    UINT                               m_allocIndex = 0;
    UINT64                             m_allocFenceValue[kCmdAllocCount] = {};
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

    // GPU zaman damgasi sorgulari. Slot basina iki damga (baslangic/bitis);
    // slot sayisi komut ayirici sayisiyla ayni, boylece BeginFrame'de zaten
    // beklenen fence okumanin hazir oldugunu da garanti eder.
    ComPtr<ID3D12QueryHeap> m_tsHeap;
    ComPtr<ID3D12Resource>  m_tsReadback;
    UINT64                  m_tsFrequency = 0;

    // Komut ayirici rotasyonu ve WaitForGpu icin B cihazina ait yerel fence.
    // (m_fenceOutD12 cross-adapter modda A cihazinda yasar, bu is icin kullanilamaz.)
    ComPtr<ID3D12Fence> m_gpuFence;
    UINT64              m_gpuFenceValue = 0;

    // ======================================================================
    // Cross-adapter kopru (yalnizca m_crossAdapter == true iken doludur)
    // ======================================================================
    bool         m_crossAdapter = false;
    std::wstring m_captureGpuName;
    std::wstring m_dlssGpuName;
    size_t       m_bridgeBytesPerFrame = 0;

    // A (yakalama GPU'su) uzerindeki D3D12 cihazi ve kopya kuyrugu.
    // Kare basina IKI ayri gonderim var (kare oncesi yukleme, kare sonrasi
    // indirme), bu yuzden ayirici/liste ciftleri de ayri tutuluyor: ayni
    // ayiriciyi ilk gonderim ucarken sifirlayamayiz.
    ComPtr<ID3D12Device>              m_srcDevice;
    ComPtr<ID3D12CommandQueue>        m_srcQueue;

    ComPtr<ID3D12CommandAllocator>    m_srcUpAlloc[kCmdAllocCount];
    ComPtr<ID3D12GraphicsCommandList> m_srcUpList;
    UINT                              m_srcUpIndex = 0;
    UINT64                            m_srcUpFenceVal[kCmdAllocCount] = {};

    ComPtr<ID3D12CommandAllocator>    m_srcDnAlloc[kCmdAllocCount];
    ComPtr<ID3D12GraphicsCommandList> m_srcDnList;
    UINT                              m_srcDnIndex = 0;
    UINT64                            m_srcDnFenceVal[kCmdAllocCount] = {};

    ComPtr<ID3D12Fence>               m_srcFence;
    UINT64                            m_srcFenceValue = 0;
    HANDLE                            m_hSrcFenceEvent = nullptr;

    // A'daki D3D11 dokularinin A-D3D12 gorunumleri.
    ComPtr<ID3D12Resource> m_srcInD12;
    ComPtr<ID3D12Resource> m_srcMvD12;
    ComPtr<ID3D12Resource> m_srcOutD12;

    // Cross-adapter paylasimli tampon. Dokular yerine TAMPON kullaniyoruz:
    // cross-adapter row-major doku destegi (CrossAdapterRowMajorTextureSupported)
    // her surucude yok, tampon her yerde var.
    //
    // heapA/bufA A cihazinda yaratilir, ayni heap B'de acilip ayni tanimla
    // ikinci bir yerlestirilmis kaynak olusturulur. Iki taraf kaynak durumunu
    // ayri ayri izledigi icin state gecisine gerek yoktur: uretici tarafta
    // COPY_DEST, tuketici tarafta COPY_SOURCE olarak yaratilir ve hic degismez.
    struct XABuffer
    {
        ComPtr<ID3D12Heap>                 heapA, heapB;
        ComPtr<ID3D12Resource>             bufA,  bufB;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};   // dokunun tampon icindeki ayak izi
        UINT64                             sizeBytes = 0;

        void Reset()
        {
            bufB.Reset();  bufA.Reset();
            heapB.Reset(); heapA.Reset();
            fp = {};
            sizeBytes = 0;
        }
    };

    XABuffer m_xaIn;    // A -> B : yakalanan/olceklenen kare (BGRA8)
    XABuffer m_xaMv;    // A -> B : hareket vektorleri (R16G16_FLOAT)
    XABuffer m_xaOut;   // B -> A : DLSS ciktisi (BGRA8)

    // Cross-adapter fence ciftleri (ayni fence, iki cihazda acilmis hali).
    ComPtr<ID3D12Fence> m_xaFenceInA,  m_xaFenceInB;    // A -> B
    ComPtr<ID3D12Fence> m_xaFenceOutA, m_xaFenceOutB;   // B -> A

    UINT64 m_frameIndex = 0;
    double m_lastMvMs   = 0.0;
    double m_lastGpuMs  = 0.0;
    bool   m_useDirectSharedOut  = false;
    bool   m_enableExplicitFlush = false;
};
