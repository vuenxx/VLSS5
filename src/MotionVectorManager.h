#pragma once
#include "Common.h"

// ---------------------------------------------------------------------------
// MotionVectorManager
//   High-performance DirectCompute-based Optical Flow and Dynamic UI
//   Reactive Mask generator for NVIDIA DLSS.
// ---------------------------------------------------------------------------
class MotionVectorManager
{
public:
    MotionVectorManager() = default;
    ~MotionVectorManager() { Cleanup(); }

    bool Init(ID3D11Device* device, int width, int height);
    void Resize(ID3D11Device* device, int width, int height);
    void Cleanup();

    // Computes Motion Vectors and Dynamic Reactive Mask from the captured frame
    bool ProcessFrame(
        ID3D11DeviceContext* ctx,
        ID3D11ShaderResourceView* currentFrameSRV,
        ID3D11Texture2D* currentFrameTex = nullptr,
        ID3D11UnorderedAccessView* targetMvUAV = nullptr,
        ID3D11Texture2D* targetMvTex = nullptr);

    ID3D11ShaderResourceView*  GetMotionVectorsSRV()     const { return m_mvSRV.Get(); }
    ID3D11Texture2D*           GetMotionVectorsTexture() const { return m_mvTexture.Get(); }

    ID3D11ShaderResourceView*  GetUiMaskSRV()            const { return m_maskSRV.Get(); }
    ID3D11Texture2D*           GetUiMaskTexture()        const { return m_maskTexture.Get(); }

    ID3D11Texture2D*           GetDepthTexture()         const { return m_depthTexture.Get(); }
    ID3D11ShaderResourceView*  GetDepthSRV()             const { return m_depthSRV.Get(); }

    void SetUiMaskEnabled(bool enabled) { m_uiMaskEnabled = enabled; }
    bool IsUiMaskEnabled() const { return m_uiMaskEnabled; }

    int GetWidth()  const { return m_width;  }
    int GetHeight() const { return m_height; }

private:
    void CleanupTextures();
    bool CreateResources(ID3D11Device* device, int width, int height);
    bool CompileShaders(ID3D11Device* device);
    void ProbeMotionVectors(ID3D11DeviceContext* ctx, ID3D11Texture2D* mvTex);

    struct ComputeCB
    {
        float screenSize[2];
        float invScreenSize[2];
        int   uiMaskEnabled;
        float uiMaskThreshold;
        float padding[2];
    };

    int   m_width            = 0;
    int   m_height           = 0;
    bool  m_firstFrame       = true;
    bool  m_uiMaskEnabled    = true;
    float m_uiMaskThreshold  = 0.08f;

    ComPtr<ID3D11Texture2D>           m_prevFrameTexture;
    ComPtr<ID3D11ShaderResourceView>  m_prevFrameSRV;

    ComPtr<ID3D11Texture2D>           m_mvTexture;
    ComPtr<ID3D11ShaderResourceView>  m_mvSRV;
    ComPtr<ID3D11UnorderedAccessView> m_mvUAV;

    ComPtr<ID3D11Texture2D>           m_maskTexture;
    ComPtr<ID3D11ShaderResourceView>  m_maskSRV;
    ComPtr<ID3D11UnorderedAccessView> m_maskUAV;

    ComPtr<ID3D11Texture2D>           m_depthTexture;
    ComPtr<ID3D11ShaderResourceView>  m_depthSRV;

    ComPtr<ID3D11Texture2D>           m_stagingMv;

    ComPtr<ID3D11ComputeShader>       m_opticalFlowCS;
    ComPtr<ID3D11Buffer>              m_constantBuffer;
    ComPtr<ID3D11SamplerState>        m_linearSampler;
};
