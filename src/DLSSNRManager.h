#pragma once
#include "Common.h"
#include <d3d12.h>
#include <dxgi1_6.h>
#include <string>

// Function pointer typedefs for D3D12 NGX Core
using PFN_NVSDK_NGX_D3D12_Init_Ext = int(__cdecl*)(
    unsigned long long appId, const wchar_t* dataPath, ID3D12Device* device,
    int sdkVersion, const void* featureInfo);
using PFN_NVSDK_NGX_D3D12_GetCapabilityParameters = int(__cdecl*)(void** params);
using PFN_NVSDK_NGX_D3D12_AllocateParameters = int(__cdecl*)(void** params);
using PFN_NVSDK_NGX_D3D12_DestroyParameters = int(__cdecl*)(void* params);

// Forwarder function pointer typedefs
using PFN_dlssnr_call_create = void* (__cdecl*)(
    const wchar_t* snippetPath, const wchar_t* dataPath,
    ID3D12Device* device, ID3D12GraphicsCommandList* cmd,
    void* capabilityParams, unsigned int width,
    unsigned int height, int preset, float intensity,
    int style, float localStructure, float localTone,
    float skinStructure, int useAutoMask,
    int uiCorrection);

using PFN_dlssnr_call_evaluate = int(__cdecl*)(
    ID3D12GraphicsCommandList* cmd, void* feature,
    void* capabilityParams, ID3D12Resource* color,
    ID3D12Resource* depth, ID3D12Resource* motion,
    ID3D12Resource* output, unsigned int width,
    unsigned int height, unsigned int guideWidth,
    unsigned int guideHeight, int depthInverted, int reset,
    float intensity, int style, float localStructure,
    float localTone, float skinStructure, int useAutoMask,
    float mvScaleX, float mvScaleY);

using PFN_dlssnr_call_release = void(__cdecl*)(void* feature);
using PFN_dlssnr_call_set_float_slot = void(__cdecl*)(int slot);

class DLSSNRManager
{
public:
    DLSSNRManager() = default;
    ~DLSSNRManager() { Cleanup(); }

    bool Init(ID3D12Device* device, ID3D12CommandQueue* queue, int width, int height);
    void Cleanup();
    bool Resize(int width, int height);

    // Evaluate DLSS 5 Neural Rendering on the given command list
    bool Evaluate(
        ID3D12GraphicsCommandList* cmdList,
        ID3D12Resource* inputColor,
        ID3D12Resource* outputRes,
        ID3D12Resource* motionVectors = nullptr);

    // Settings
    void  ApplyConfig(const struct Dlss5Config& cfg);

    bool  IsEnabled()           const { return m_enabled; }
    void  SetEnabled(bool v)          { m_enabled = v; if (v) m_firstFrame = true; }
    void  ResetHistory()              { m_firstFrame = true; }

    float GetIntensity()        const { return m_intensity; }
    void  SetIntensity(float v)       { m_intensity = v;    }

    int   GetPreset()           const { return m_preset; }
    void  SetPreset(int v)            { m_preset = v; m_needsRebuild = true; }

    int   GetStyle()            const { return m_style; }
    void  SetStyle(int v)             { m_style = v; m_needsRebuild = true; }

    float GetLocalStructure()   const { return m_localStructure; }
    void  SetLocalStructure(float v)  { m_localStructure = v;    }

    float GetLocalTone()        const { return m_localTone; }
    void  SetLocalTone(float v)       { m_localTone = v;    }

    float GetSkinStructure()    const { return m_skinStructure; }
    void  SetSkinStructure(float v)   { m_skinStructure = v;    }

    float GetResolutionScale()  const { return m_resolutionScale; }
    int   GetWorkWidth()        const { return m_workWidth;  }
    int   GetWorkHeight()       const { return m_workHeight; }
    bool  IsTemporalStabilizer() const { return m_temporalStabilizer; }
    void  SetTemporalStabilizer(bool v) { m_temporalStabilizer = v; }
    bool  IsOpticalFlow()       const { return m_opticalFlow; }
    void  SetOpticalFlow(bool v)      { m_opticalFlow = v; }
    bool  IsEvaluating()        const { return m_isEvaluating; }
    int   GetLastEvalResult()   const { return m_lastEvalResult; }
    int   GetWidth()            const { return m_width;  }
    int   GetHeight()           const { return m_height; }

private:
    bool LoadNGXCore();
    bool LoadForwarder();
    void DiscoverAndSetFloatSlot();
    bool CreateGuideTextures(int width, int height);
    bool CreateFeature();
    void ReleaseFeature();

    int   m_width           = 0;
    int   m_height          = 0;
    int   m_workWidth       = 0;
    int   m_workHeight      = 0;
    float m_resolutionScale = 1.0f;

    bool      m_enabled               = true;
    float     m_intensity             = 1.0f;
    int       m_preset                = 0;
    int       m_style                 = 0;
    float     m_localStructure        = 0.0f;
    float     m_localTone             = 0.0f;
    float     m_skinStructure         = 0.0f;
    int       m_useAutoMask           = 0;
    int       m_uiCorrection          = 0;
    bool      m_depthInverted         = false;
    bool      m_needsRebuild          = false;
    bool      m_temporalStabilizer    = false;
    bool      m_opticalFlow           = true;
    ULONGLONG m_lastConfigChangeTime  = 0;
    bool      m_firstFrame            = true;

    bool  m_isEvaluating    = false;
    int   m_lastEvalResult  = 0;

    // Device references
    ID3D12Device*       m_device = nullptr;
    ID3D12CommandQueue* m_queue  = nullptr;

    // NGX Core pointers
    HMODULE                                     m_hNgxCore = nullptr;
    PFN_NVSDK_NGX_D3D12_Init_Ext                m_pfnInitExt = nullptr;
    PFN_NVSDK_NGX_D3D12_GetCapabilityParameters m_pfnGetCaps = nullptr;
    PFN_NVSDK_NGX_D3D12_AllocateParameters      m_pfnAllocParams = nullptr;
    PFN_NVSDK_NGX_D3D12_DestroyParameters       m_pfnDestroyParams = nullptr;

    void* m_params = nullptr;

    // Forwarder pointers
    HMODULE                        m_hForwarder = nullptr;
    PFN_dlssnr_call_create         m_pfnCreate = nullptr;
    PFN_dlssnr_call_evaluate       m_pfnEvaluate = nullptr;
    PFN_dlssnr_call_release        m_pfnRelease = nullptr;
    PFN_dlssnr_call_set_float_slot m_pfnSetFloatSlot = nullptr;
    int*                           m_pLastInit = nullptr;
    int*                           m_pLastCreate = nullptr;

    // Active Feature Handle
    void* m_feature = nullptr;

    // Guide textures for DLSS-NR model
    ComPtr<ID3D12Resource> m_depthTex;
    ComPtr<ID3D12Resource> m_motionTex;

    std::wstring m_snippetPath;
    std::wstring m_dataPath;
};
