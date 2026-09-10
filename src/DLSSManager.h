#pragma once
#include "Common.h"

// ---------------------------------------------------------------------------
// NVIDIA NGX Types & Interfaces (Zero-dependency definition)
// ---------------------------------------------------------------------------
typedef struct NVSDK_NGX_Handle { unsigned int Id; } NVSDK_NGX_Handle;

enum NVSDK_NGX_Feature
{
    NVSDK_NGX_Feature_SuperSampling = 1,
    NVSDK_NGX_Feature_Reserved18    = 18 // Neural Rendering / DLSS-NR
};

enum NVSDK_NGX_PerfQuality_Value
{
    NVSDK_NGX_PerfQuality_Value_MaxPerf          = 0,
    NVSDK_NGX_PerfQuality_Value_Balanced         = 1,
    NVSDK_NGX_PerfQuality_Value_MaxQuality       = 2,
    NVSDK_NGX_PerfQuality_Value_UltraPerformance = 3,
    NVSDK_NGX_PerfQuality_Value_UltraQuality     = 4,
    NVSDK_NGX_PerfQuality_Value_DLAA             = 5
};

enum NVSDK_NGX_DLSS_Feature_Flags
{
    NVSDK_NGX_DLSS_Feature_Flags_None          = 0,
    NVSDK_NGX_DLSS_Feature_Flags_IsHDR         = 1 << 0, // 1
    NVSDK_NGX_DLSS_Feature_Flags_MVLowRes      = 1 << 1, // 2
    NVSDK_NGX_DLSS_Feature_Flags_MVJittered    = 1 << 2, // 4
    NVSDK_NGX_DLSS_Feature_Flags_DepthInverted = 1 << 3, // 8
    NVSDK_NGX_DLSS_Feature_Flags_Reserved_0    = 1 << 4, // 16
    NVSDK_NGX_DLSS_Feature_Flags_DoSharpening  = 1 << 5, // 32
    NVSDK_NGX_DLSS_Feature_Flags_AutoExposure  = 1 << 6  // 64
};

struct NVSDK_NGX_Parameter
{
    virtual void Set(const char* InName, unsigned long long InValue) = 0;
    virtual void Set(const char* InName, float InValue) = 0;
    virtual void Set(const char* InName, double InValue) = 0;
    virtual void Set(const char* InName, unsigned int InValue) = 0;
    virtual void Set(const char* InName, int InValue) = 0;
    virtual void Set(const char* InName, ID3D11Resource* InValue) = 0;
    virtual void Set(const char* InName, struct ID3D12Resource* InValue) = 0;
    virtual void Set(const char* InName, void* InValue) = 0;
    virtual unsigned int Get(const char* InName, unsigned long long* OutValue) const = 0;
    virtual unsigned int Get(const char* InName, float* OutValue) const = 0;
    virtual unsigned int Get(const char* InName, double* OutValue) const = 0;
    virtual unsigned int Get(const char* InName, unsigned int* OutValue) const = 0;
    virtual unsigned int Get(const char* InName, int* OutValue) const = 0;
    virtual unsigned int Get(const char* InName, ID3D11Resource** OutValue) const = 0;
    virtual unsigned int Get(const char* InName, struct ID3D12Resource** OutValue) const = 0;
    virtual unsigned int Get(const char* InName, void** OutValue) const = 0;
    virtual void Reset() = 0;
};

void DLSS_Log(const char* fmt, ...);

// ---------------------------------------------------------------------------
// DLSSManager
//   Native NVIDIA NGX integration for DLSS and Neural Rendering.
//   Bypasses ReShade completely for maximum performance and zero UI ghosting.
// ---------------------------------------------------------------------------
class DLSSManager
{
public:
    DLSSManager() = default;
    ~DLSSManager() { Cleanup(); }

    bool Init(ID3D11Device* device, ID3D11DeviceContext* ctx, int width, int height);
    void Resize(ID3D11Device* device, ID3D11DeviceContext* ctx, int width, int height);
    void Cleanup();

    // Runs DLSS on the captured frame using motion vectors and reactive mask.
    // Returns the upscaled/enhanced texture SRV.
    // Returns inputSRV if DLSS is unavailable or failed.
    ID3D11ShaderResourceView* Evaluate(
        ID3D11DeviceContext*     ctx,
        ID3D11ShaderResourceView* inputSRV,
        ID3D11Texture2D*          inputTexture,
        ID3D11Texture2D*          mvTexture,
        ID3D11Texture2D*          depthTexture,
        ID3D11Texture2D*          reactiveMaskTexture);

    bool IsAvailable() const { return m_available; }
    bool IsEnabled()   const { return m_enabled;   }
    void SetEnabled(bool enabled) { m_enabled = enabled; }

    float GetSharpness() const { return m_sharpness; }
    void  SetSharpness(float sharpness) { m_sharpness = sharpness; }

    int GetWidth()  const { return m_width;  }
    int GetHeight() const { return m_height; }

private:
    bool LoadNGXLibrary();
    bool CreateFeature(ID3D11Device* device, ID3D11DeviceContext* ctx, int width, int height);
    void ReleaseFeature();

    // Function pointers to _nvngx.dll
    typedef unsigned int (*PFN_NVSDK_NGX_D3D11_Init)(
        unsigned long long InAppId, const wchar_t* InDataPath, ID3D11Device* InDevice,
        const void* InFeatureInfo, unsigned int InSDKVersion);
    typedef unsigned int (*PFN_NVSDK_NGX_D3D11_AllocateParameters)(NVSDK_NGX_Parameter** OutParams);
    typedef unsigned int (*PFN_NVSDK_NGX_D3D11_GetCapabilityParameters)(NVSDK_NGX_Parameter** OutParams);
    typedef unsigned int (*PFN_NVSDK_NGX_D3D11_DestroyParameters)(NVSDK_NGX_Parameter* InParams);
    typedef unsigned int (*PFN_NVSDK_NGX_D3D11_CreateFeature)(
        ID3D11DeviceContext* InDevCtx, NVSDK_NGX_Feature InFeatureId,
        const NVSDK_NGX_Parameter* InParams, NVSDK_NGX_Handle** OutHandle);
    typedef unsigned int (*PFN_NVSDK_NGX_D3D11_EvaluateFeature)(
        ID3D11DeviceContext* InDevCtx, const NVSDK_NGX_Handle* InFeatureHandle,
        const NVSDK_NGX_Parameter* InParams, void* InCallback);
    typedef unsigned int (*PFN_NVSDK_NGX_D3D11_ReleaseFeature)(NVSDK_NGX_Handle* InHandle);
    typedef unsigned int (*PFN_NVSDK_NGX_D3D11_Shutdown)(void);

    HMODULE m_hNgxDll = nullptr;
    PFN_NVSDK_NGX_D3D11_Init                   m_pfnInit = nullptr;
    PFN_NVSDK_NGX_D3D11_AllocateParameters     m_pfnAllocParams = nullptr;
    PFN_NVSDK_NGX_D3D11_GetCapabilityParameters m_pfnGetCaps = nullptr;
    PFN_NVSDK_NGX_D3D11_DestroyParameters      m_pfnDestroyParams = nullptr;
    PFN_NVSDK_NGX_D3D11_CreateFeature          m_pfnCreateFeature = nullptr;
    PFN_NVSDK_NGX_D3D11_EvaluateFeature        m_pfnEvaluateFeature = nullptr;
    PFN_NVSDK_NGX_D3D11_ReleaseFeature         m_pfnReleaseFeature = nullptr;
    PFN_NVSDK_NGX_D3D11_Shutdown               m_pfnShutdown = nullptr;

    NVSDK_NGX_Parameter* m_ngxParams    = nullptr;
    NVSDK_NGX_Handle*    m_dlssFeature  = nullptr;

    ComPtr<ID3D11Texture2D>          m_outputTexture;
    ComPtr<ID3D11ShaderResourceView> m_outputSRV;

    int   m_width     = 0;
    int   m_height    = 0;
    bool  m_available = false;
    bool  m_enabled   = true;
    bool  m_reset     = true;
    float m_sharpness = 0.5f;
    uint64_t m_evaluateCount = 0;
};
