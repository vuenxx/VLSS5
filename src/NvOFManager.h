#pragma once
#include "Common.h"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <memory>
#include "nvOpticalFlowD3D11.h"

// ---------------------------------------------------------------------------
// NvOFManager
//   Hardware-accelerated NVIDIA Optical Flow (OFA) engine wrapper using
//   NVIDIA Optical Flow SDK 5.0 (nvofapi64.dll) on D3D11.
//   Executes on dedicated silicon with ZERO GPU SM/CUDA core utilization.
// ---------------------------------------------------------------------------
class NvOFManager
{
public:
    NvOFManager() = default;
    ~NvOFManager() { Cleanup(); }

    // Initializes the hardware NVOF session and pre-registers GPU textures
    bool Init(ID3D11Device* device, ID3D11DeviceContext* context, int width, int height);

    // Recreates resources and updates NVOF registration on resolution change
    void Resize(ID3D11Device* device, ID3D11DeviceContext* context, int width, int height);

    // Releases all hardware handles, registered buffers, and library instances
    void Cleanup();

    // Executes hardware optical flow and converts S16.5 vectors directly into targetMvUAV (R16G16_FLOAT)
    // NOTE: Does NOT perform resource registration per frame (registration is done once in Init/Resize).
    bool ProcessFrame(
        ID3D11DeviceContext* context,
        ID3D11Texture2D* currentFrameTex,
        ID3D11Texture2D* prevFrameTex,
        ID3D11UnorderedAccessView* targetMvUAV,
        ID3D11Texture2D* targetMvTex = nullptr);

    bool IsSupported() const { return m_initialized; }
    int  GetWidth()    const { return m_width;  }
    int  GetHeight()   const { return m_height; }

    ID3D11Texture2D* GetRawOutputTexture() const { return m_rawMvTex.Get(); }

private:
    bool LoadNVOFEntryPoints();
    bool CreateAndRegisterResources(ID3D11Device* device, int width, int height);
    void UnregisterResources();
    bool CompileConvertShader(ID3D11Device* device);

    int  m_width       = 0;
    int  m_height      = 0;
    bool m_initialized = false;

    // Library and function pointers
    HMODULE                       m_hNvOfDll = nullptr;
    NV_OF_D3D11_API_FUNCTION_LIST m_nvof     = { 0 };
    NvOFHandle                    m_hOf      = nullptr;

    // Hardware registered textures and handles (Created & Registered ONCE in Init/Resize)
    ComPtr<ID3D11Texture2D>       m_curInputTex;
    NvOFGPUBufferHandle           m_hCurInputBuffer = nullptr;

    ComPtr<ID3D11Texture2D>       m_prevInputTex;
    NvOFGPUBufferHandle           m_hPrevInputBuffer = nullptr;

    ComPtr<ID3D11Texture2D>          m_rawMvTex;
    ComPtr<ID3D11ShaderResourceView> m_rawMvSRV;
    NvOFGPUBufferHandle              m_hRawMvBuffer = nullptr;

    // Format converter compute shader (S16.5 fixed-point -> R16G16_FLOAT pixels)
    ComPtr<ID3D11ComputeShader>   m_convertCS;
};
