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

    // Evaluate DLSS 5 Neural Rendering on the given command list.
    // forceReset: caganin (Renderer/MotionVectorManager) fotometrik guven gibi
    // KENDI olcumune dayanarak temporal gecmisi kirmak istedigini bildirmesi icin.
    bool Evaluate(
        ID3D12GraphicsCommandList* cmdList,
        ID3D12Resource* inputColor,
        ID3D12Resource* outputRes,
        ID3D12Resource* motionVectors = nullptr,
        bool forceReset = false);

    // Settings
    void  ApplyConfig(const struct Dlss5Config& cfg);

    bool  IsEnabled()           const { return m_enabled; }
    void  SetEnabled(bool v);
    void  ResetHistory()              { m_firstFrame = true; }

    void  MarkConfigChanged()         { m_needsRebuild = true; m_lastConfigChangeTime = GetTickCount64(); }

    float GetIntensity()        const { return m_intensity; }
    void  SetIntensity(float v)       { if (m_intensity != v) { m_intensity = v; MarkConfigChanged(); } }

    int   GetPreset()           const { return m_preset; }
    void  SetPreset(int v)            { if (m_preset != v) { m_preset = v; MarkConfigChanged(); } }

    int   GetStyle()            const { return m_style; }
    void  SetStyle(int v)             { if (m_style != v) { m_style = v; MarkConfigChanged(); } }

    float GetLocalStructure()   const { return m_localStructure; }
    void  SetLocalStructure(float v)  { if (m_localStructure != v) { m_localStructure = v; MarkConfigChanged(); } }

    float GetLocalTone()        const { return m_localTone; }
    void  SetLocalTone(float v)       { if (m_localTone != v) { m_localTone = v; MarkConfigChanged(); } }

    float GetSkinStructure()    const { return m_skinStructure; }
    void  SetSkinStructure(float v)   { if (m_skinStructure != v) { m_skinStructure = v; MarkConfigChanged(); } }

    // --- Cok Gecisli (Multipass) Noral Isleme -------------------------------
    // Model kendi ciktisini tekrar girdi olarak alip N kez calisir. Her gecisin
    // AYRI feature handle'i vardir: NGX ozelligi kare-icinde temporal gecmis
    // tutar, tek handle'i ayni karede tekrar cagirmak o gecmisi birbirine
    // karistirir ve sonuc detay kazanmak yerine yalnizca renk kaymasi olur.
    // Ayri handle'larda k. gecis her karede AYNI kaynagi (k-1. gecisin ciktisi)
    // gordugu icin kendi gecmisi tutarli kalir ve detay gercekten birikir.
    int   GetPassCount()        const { return m_passCount; }
    void  SetPassCount(int v)
    {
        if (v < 1) v = 1;
        if (v > kMaxPasses) v = kMaxPasses;
        if (m_passCount != v) { m_passCount = v; MarkConfigChanged(); }
    }

    float GetPassFalloff()      const { return m_passFalloff; }
    void  SetPassFalloff(float v)     { if (m_passFalloff != v) m_passFalloff = v; }

    float GetResolutionScale()  const { return m_resolutionScale; }
    int   GetWorkWidth()        const { return m_workWidth;  }
    int   GetWorkHeight()       const { return m_workHeight; }

    // GetWorkWidth/Height HEDEF (istenen) calisma cozunurlugudur; olcek
    // slider'i degistirilir degistirilmez, feature/guide dokulari yeniden
    // kurulmadan (400ms debounce, bkz. Evaluate()) ONCE guncellenir. Eger
    // D3D12Interop/MotionVectorManager bu HEDEF boyuta gore hemen resize
    // edilirse, feature hala ESKI boyutta kurulu oldugu icin Evaluate()
    // NGX'e "bu WxH" der ama iceride gercek doku ESKI boyutta kalir --
    // gecis penceresinde (400ms) ciktida gorsel bozulma/hayalet olusur.
    // GetBuiltWorkWidth/Height, feature'in GERCEKTEN kurulu oldugu boyutu
    // dondurur; caginin bunu kullanmasi bu yarisi tamamen ortadan kaldirir.
    int   GetBuiltWorkWidth()   const { return m_builtWorkWidth;  }
    int   GetBuiltWorkHeight()  const { return m_builtWorkHeight; }
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
    bool CreateChainTextures(int width, int height);
    bool CreateFeature();
    void ReleaseFeature();

    // Tek bir gecisi komut listesine yazar. Cagrilar arasinda kaynak durumlari
    // COMMON'a geri dondugu icin bir sonraki gecis oncekinin ciktisini guvenle
    // okur -- COMMON'a gecis bariyeri o kaynak icin yazma gorunurlugunu saglar.
    bool EvaluateSinglePass(
        ID3D12GraphicsCommandList* cmdList,
        void* feature,
        ID3D12Resource* inputColor,
        ID3D12Resource* outputRes,
        ID3D12Resource* motionVectors,
        int   reset,
        float intensity);

    int   m_width           = 0;
    int   m_height          = 0;
    int   m_workWidth       = 0;
    int   m_workHeight      = 0;
    int   m_builtWorkWidth  = 0;
    int   m_builtWorkHeight = 0;
    float m_resolutionScale = 1.0f;

    bool      m_enabled               = true;
    float     m_intensity             = 1.0f;
    int       m_preset                = 0;
    int       m_style                 = 0;
    float     m_localStructure        = 0.0f;
    float     m_localTone             = 1.0f;
    float     m_skinStructure         = 0.0f;
    int       m_useAutoMask           = 0;
    int       m_uiCorrection          = 0;
    bool      m_depthInverted         = false;
    bool      m_needsRebuild          = false;
    int       m_passCount             = 1;
    float     m_passFalloff           = 1.0f;
    bool      m_temporalStabilizer    = false;
    bool      m_opticalFlow           = true;
    ULONGLONG m_lastConfigChangeTime  = 0;
    bool      m_firstFrame            = true;

    // Evaluate cagrilari arasindaki QPC farki: sahne kesmesi / duraklama (alt-tab,
    // yukleme ekrani, olum ekrani) yakalamayi uzun sure durdurur, NvOF o arada
    // CALISMAZ ve geri donen kare ciftinde vektorler comp olur. Bu araligi
    // Evaluate()'in GIRISINDE olcup mantikli aralik disindaysa temporal gecmisi
    // burada kirmak yeterli. Esikler FramePacer.cpp'deki 0.5/200 ms ile ayni.
    LARGE_INTEGER m_qpcFreq     = {};
    LARGE_INTEGER m_lastEvalQpc = {};

    bool  m_isEvaluating    = false;
    int   m_lastEvalResult  = 0;
    int   m_consecutiveFailures = 0;

    // Device references
    ID3D12Device*       m_device = nullptr;
    ID3D12CommandQueue* m_queue  = nullptr;

    // ---- process-omurlu, TUM DLSSNRManager ornekleri arasinda PAYLASILAN durum ----
    // NEDEN static: her "Baslat/Durdur" oturumunda Renderer (dolayisiyla
    // DLSSNRManager) SIFIRDAN bir nesne olarak yeniden kuruluyordu. Eskiden
    // Cleanup() m_params'i yok ediyordu (m_hNgxCore/m_hForwarder DLL
    // handle'lari zaten hicbir zaman FreeLibrary edilmiyordu -- NGX'in arka
    // plan thread'leri yuzunden bu guvensiz, bkz. asagidaki not) -- bu da
    // LoadNGXCore()'daki "zaten yuklu mu" korumasini (m_hNgxCore && m_params)
    // her YENI ornekte anlamsiz kiliyordu (yeni ornegin m_params'i hep null
    // baslar) ve NVSDK_NGX_D3D12_Init_Ext SÜREÇ ICINDE HALA CANLI olan NGX
    // calisma zamanina karsi İKİNCİ/UCUNCU kez cagriliyordu -- muhtemelen 2.
    // acilistaki fazladan donma/bozulmaya katkisi bu. D3D12Interop'un cihazi
    // da artik ayni sebeple kalici oldugundan (bkz. D3D12Interop.h), Init_Ext'i
    // GERCEKTEN o kalici cihaza karsi sadece BIR KEZ cagirip s_ngxDevice ile
    // izliyoruz; farkli bir cihaza gecilirse (GPU tercihi degisti) yeniden
    // cagiriyoruz.
    //
    // NOT: Ayni "hic Shutdown/FreeLibrary cagirma" deseni burada da geçerli --
    // NVIDIA NGX arka plan thread'leri surdurdugunden bunlari kapatmak/bosaltmak
    // 0xC0000005 cokmesine yol acabilir (bkz. DLSSManager.cpp'deki ayni not).
    static ID3D12Device*                               s_ngxDevice;
    static HMODULE                                     s_hNgxCore;
    static PFN_NVSDK_NGX_D3D12_Init_Ext                s_pfnInitExt;
    static PFN_NVSDK_NGX_D3D12_GetCapabilityParameters  s_pfnGetCaps;
    static PFN_NVSDK_NGX_D3D12_AllocateParameters       s_pfnAllocParams;
    static PFN_NVSDK_NGX_D3D12_DestroyParameters        s_pfnDestroyParams;
    static void*                                        s_params;

    // Forwarder pointers -- cihazdan bagimsiz (sadece DLL yukleme/GetProcAddress),
    // bu yuzden herhangi bir cihaz karsilastirmasina gerek yok, bir kez yeter.
    static HMODULE                        s_hForwarder;
    static PFN_dlssnr_call_create         s_pfnCreate;
    static PFN_dlssnr_call_evaluate       s_pfnEvaluate;
    static PFN_dlssnr_call_release        s_pfnRelease;
    static PFN_dlssnr_call_set_float_slot s_pfnSetFloatSlot;
    static int*                           s_pLastInit;
    static int*                           s_pLastCreate;

    // Aktif Feature Handle'lari -- gecis basina bir tane (indeks 0 = ilk gecis).
    static constexpr int kMaxPasses = 4;
    void* m_features[kMaxPasses] = {};

    // Gecisler arasi ping-pong dokulari (work cozunurlugu, native D3D12).
    // Paylasimli degil: ara sonuclarin D3D11 tarafinda gorunmesi gerekmiyor,
    // bu da gecis basina ek bir paylasim/senkron bedeli olmamasi demek.
    ComPtr<ID3D12Resource> m_chainTex[2];
    int m_chainWidth  = 0;
    int m_chainHeight = 0;

    // Guide textures for DLSS-NR model
    ComPtr<ID3D12Resource> m_depthTex;
    ComPtr<ID3D12Resource> m_motionTex;

    std::wstring m_snippetPath;
    std::wstring m_dataPath;
};
