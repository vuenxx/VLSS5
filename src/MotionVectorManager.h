#pragma once
#include "Common.h"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <memory>
#include "NvOFManager.h"

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

    bool IsHardwareNvOFActive() const { return m_useHardwareNvOF; }

    int GetWidth()  const { return m_width;  }
    int GetHeight() const { return m_height; }

    // Fotometrik kare-guveni: akisin GERCEKTEN dogru olup olmadigini olcer (Probe
    // yalnizca buyukluk raporlar, dogruluk hakkinda bir sey soylemez). Async ring
    // readback kullandigi icin kConfRingSize kare (~3 kare) gecikmelidir.
    bool  IsFlowSuspect()               const { return m_flowSuspect; }
    float GetLastPhotoConfidenceError() const { return m_lastPhotoConfErr; }

private:
    void CleanupTextures();
    bool CreateResources(ID3D11Device* device, int width, int height);
    bool CompileShaders(ID3D11Device* device);
    bool CompilePhotoConfidenceShader(ID3D11Device* device);
    void ProbeMotionVectors(ID3D11DeviceContext* ctx, ID3D11Texture2D* mvTex);
    void UpdatePhotoConfidence(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* currentFrameSRV, ID3D11Texture2D* mvTex);

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

    // Tek 16x16 merkez blok yerine kareye yayilmis seyrek izgara: merkez blok
    // TEK bir hareket yonunu ornekliyordu, bu yuzden p50/max hep esitti ve akis
    // alaninin geri kalani hakkinda hicbir sey soylemiyordu (yanlis teshise yol
    // acti). 32x18 = 576 nokta, her biri karenin farkli bir hucresinde.
    static constexpr int              kProbeGridW = 32;
    static constexpr int              kProbeGridH = 18;
    static constexpr int              kProbeRingSize = 3;
    ComPtr<ID3D11Texture2D>           m_stagingMvRing[kProbeRingSize];
    int                               m_probeRingIndex = 0;
    int                               m_probeFramesPending = 0;

    ComPtr<ID3D11ComputeShader>       m_opticalFlowCS;
    ComPtr<ID3D11Buffer>              m_constantBuffer;
    ComPtr<ID3D11SamplerState>        m_linearSampler;

    std::unique_ptr<NvOFManager>      m_nvof;
    bool                              m_useHardwareNvOF = false;

    // --- Fotometrik kare-guveni (Gorev 4) -----------------------------------
    // QPC bosluk testi (DLSSNRManager) yalnizca "capture durdu mu" sorusuna
    // bakar; bu ise "vektorler GERCEKTEN dogru mu" sorusuna bakar ve capture
    // hic durmadan da olan ani sahne degisimlerini (kesme, patlama flası vb.)
    // yakalar. Ayni 32x18 izgarayi (kProbeGridW/H) kullanir ama Probe'un
    // aksine HER karede calisir -- sahne kesmesi 300 karede birde degil,
    // herhangi bir karede olabilir.
    ComPtr<ID3D11ComputeShader>       m_photoConfCS;
    ComPtr<ID3D11Buffer>              m_photoConfCB;
    ComPtr<ID3D11Texture2D>           m_confTexture;   // 32x18 R32_FLOAT UAV (GPU sonucu)
    ComPtr<ID3D11UnorderedAccessView> m_confUAV;

    // DLSS-NR ile hardware NvOF kullanilirken hareket vektorleri D3D12Interop'un
    // PAYLASIMLI D3D11 dokusuna yaziliyor (m_mvTexture DEGIL). O dokunun SRV'si
    // bize verilmiyor; pointer degismedigi surece burada bir kez olusturup
    // onbellekliyoruz (her karede yeni view olusturmak pahali olurdu).
    ComPtr<ID3D11ShaderResourceView>  m_externalMvSRV;
    ID3D11Texture2D*                  m_cachedExternalMvTexPtr = nullptr;

    static constexpr int              kConfRingSize = 3;
    ComPtr<ID3D11Texture2D>           m_confStagingRing[kConfRingSize];
    int                               m_confRingIndex = 0;
    int                               m_confFramesPending = 0;

    bool  m_flowSuspect      = false; // en son okunan (~kConfRingSize kare gecikmeli) sonuc
    float m_lastPhotoConfErr = 0.0f;
};
