#include "MotionVectorManager.h"
#include "ConfigManager.h"
#include <algorithm>

// bkz. MotionVectorManager.h'deki aciklama: process-omurlu, oturumlar arasi paylasilan
// derlenmis shader'lar.
ComPtr<ID3D11ComputeShader> MotionVectorManager::s_opticalFlowCS;
ComPtr<ID3D11ComputeShader> MotionVectorManager::s_photoConfCS;

// ---------------------------------------------------------------------------
// Compute Shader for Optical Flow & Dynamic UI Reactive Mask
// ---------------------------------------------------------------------------
static const char* kOpticalFlowCSSource = R"HLSL(
Texture2D<float4>   g_CurrentFrame   : register(t0);
Texture2D<float4>   g_PrevFrame      : register(t1);
SamplerState        g_Sampler        : register(s0);

RWTexture2D<float2> g_MotionVectors  : register(u0);
RWTexture2D<float>  g_ReactiveMask   : register(u1);

cbuffer Config : register(b0)
{
    float2 g_screenSize;
    float2 g_invScreenSize;
    int    g_uiMaskEnabled;
    float  g_uiMaskThreshold;
    float2 g_padding;
};

// 3x3 Block Matching Mean Squared Error (MSE) helper
// Evaluates a 3x3 neighborhood to avoid aperture problem and single-pixel sensor noise.
float ComputeMeanBlockSSD3x3(int2 curPos, int2 prevPos, int2 maxCoord)
{
    float total = 0.0f;
    [unroll]
    for (int ky = -1; ky <= 1; ++ky)
    {
        [unroll]
        for (int kx = -1; kx <= 1; ++kx)
        {
            int2 cp = clamp(curPos  + int2(kx, ky), int2(0, 0), maxCoord);
            int2 pp = clamp(prevPos + int2(kx, ky), int2(0, 0), maxCoord);
            float3 d = g_CurrentFrame[cp].rgb - g_PrevFrame[pp].rgb;
            total += dot(d, d);
        }
    }
    return total * 0.111111f; // Mean error per pixel in 3x3 block
}

// Groupshared memory for 8x8 block motion vector and error
groupshared int2  s_blockMV;
groupshared float s_blockErr;

[numthreads(8, 8, 1)]
void CSMain(
    uint3 gtid : SV_GroupThreadID,
    uint3 gid  : SV_GroupID,
    uint3 dtid : SV_DispatchThreadID)
{
    int2 pos = int2(dtid.xy);
    int2 maxCoord = int2((int)g_screenSize.x - 1, (int)g_screenSize.y - 1);

    // 1. Only thread (0,0) of this 8x8 block performs the hierarchical full search
    // for the center of the 8x8 block.
    if (gtid.x == 0 && gtid.y == 0)
    {
        int2 blockCenter = clamp(int2(gid.xy * 8 + int2(4, 4)), int2(0, 0), maxCoord);
        float3 curCenter  = g_CurrentFrame[blockCenter].rgb;
        float3 prevCenter = g_PrevFrame[blockCenter].rgb;
        float3 centerDiff = curCenter - prevCenter;
        float centerErr = dot(centerDiff, centerDiff);

        if (centerErr < 0.0003f)
        {
            s_blockMV  = int2(0, 0);
            s_blockErr = centerErr;
        }
        else
        {
            float centerBlockMSE = ComputeMeanBlockSSD3x3(blockCenter, blockCenter, maxCoord);
            if (centerBlockMSE < 0.00035f)
            {
                s_blockMV  = int2(0, 0);
                s_blockErr = centerBlockMSE;
            }
            else
            {
                int2 bestOffset = int2(0, 0);
                float minError  = centerBlockMSE * 0.80f;

                // Coarse search: stride 3 (-12 to 12) with fast single-pixel pre-filter
                [unroll]
                for (int dy = -12; dy <= 12; dy += 3)
                {
                    for (int dx = -12; dx <= 12; dx += 3)
                    {
                        if (dx == 0 && dy == 0) continue;
                        int2 samplePos = clamp(blockCenter + int2(dx, dy), int2(0, 0), maxCoord);
                        float3 quickDiff = curCenter - g_PrevFrame[samplePos].rgb;
                        float quickErr = dot(quickDiff, quickDiff);

                        if (quickErr < minError * 1.6f + 0.001f)
                        {
                            float err = ComputeMeanBlockSSD3x3(blockCenter, samplePos, maxCoord) + 0.00012f * (float)(abs(dx) + abs(dy));
                            if (err < minError)
                            {
                                minError   = err;
                                bestOffset = int2(dx, dy);
                            }
                        }
                    }
                }

                // Fine refinement: around best offset with stride 1
                if (bestOffset.x != 0 || bestOffset.y != 0)
                {
                    [unroll]
                    for (int fdy = -2; fdy <= 2; ++fdy)
                    {
                        for (int fdx = -2; fdx <= 2; ++fdx)
                        {
                            if (fdx == 0 && fdy == 0) continue;
                            int2 candidate = bestOffset + int2(fdx, fdy);
                            int2 samplePos = clamp(blockCenter + candidate, int2(0, 0), maxCoord);
                            float err = ComputeMeanBlockSSD3x3(blockCenter, samplePos, maxCoord) + 0.00012f * (float)(abs(candidate.x) + abs(candidate.y));
                            if (err < minError)
                            {
                                minError   = err;
                                bestOffset = candidate;
                            }
                        }
                    }
                }

                s_blockMV  = bestOffset;
                s_blockErr = minError;
            }
        }
    }

    // Synchronize all 64 threads in this 8x8 block
    GroupMemoryBarrierWithGroupSync();

    if (pos.x > maxCoord.x || pos.y > maxCoord.y)
        return;

    // 2. Per-pixel check: Static UI / HUD elements or untouched pixels
    float3 curPixel  = g_CurrentFrame[pos].rgb;
    float3 prevPixel = g_PrevFrame[pos].rgb;
    float3 pixelDiff = curPixel - prevPixel;
    float pixelCenterErr = dot(pixelDiff, pixelDiff);

    if (pixelCenterErr < 0.0003f)
    {
        g_MotionVectors[pos] = float2(0.0f, 0.0f);
        if (g_uiMaskEnabled != 0) g_ReactiveMask[pos] = 1.0f;
        return;
    }

    // 3. Ultra-fast local refinement: Each of the 64 threads tests ±1px around s_blockMV (9 samples)
    int2 baseMV = s_blockMV;
    int2 bestPixelMV = baseMV;
    float minPixelErr = 999.0f;

    [unroll]
    for (int ry = -1; ry <= 1; ++ry)
    {
        [unroll]
        for (int rx = -1; rx <= 1; ++rx)
        {
            int2 cand = baseMV + int2(rx, ry);
            int2 sPos = clamp(pos + cand, int2(0, 0), maxCoord);
            float3 d = curPixel - g_PrevFrame[sPos].rgb;
            float err = dot(d, d) + 0.00012f * (float)(abs(cand.x) + abs(cand.y));
            if (err < minPixelErr)
            {
                minPixelErr  = err;
                bestPixelMV  = cand;
            }
        }
    }

    // 4. Dynamic UI / Disocclusion Reactive Mask
    float mask = 0.0f;
    float2 finalMV = float2(bestPixelMV);

    if (g_uiMaskEnabled != 0)
    {
        float diffSamePos = length(pixelDiff);
        float mvLength    = length(finalMV);

        // If pixel is static UI or no motion detected
        if (diffSamePos < g_uiMaskThreshold || mvLength < 0.25f)
        {
            mask    = 1.0f;
            finalMV = float2(0.0f, 0.0f);
        }
        else if (minPixelErr > 0.35f)
        {
            // Disoccluded area / cutscene: no good match found
            mask    = 1.0f;
            finalMV = float2(0.0f, 0.0f);
        }
        else
        {
            mask = 0.0f;
        }
    }

    g_MotionVectors[pos] = finalMV;
    g_ReactiveMask[pos]  = mask;
}
)HLSL";

// ---------------------------------------------------------------------------
// Compute Shader for Photometric Frame Confidence (Gorev 4)
//
// Akisin ONERDIGI konumdaki renk GERCEKTEN simdiki renkle eslesiyor mu diye
// olcer. Tek piksel karsilastirmasi dokulu bolgelerde yanlis pozitif verir
// (bir komsu piksel tesadufen eslesebilir) -- 3x3 komsulukta MAKSIMUM farki
// al, ortalamayi degil. Izgara 32x18 (kProbeGridW/H ile ayni, degerler
// burada literal -- HLSL derleme zamaninda C++ sabitine erisemiyor).
// ---------------------------------------------------------------------------
static const char* kPhotoConfCSSource = R"HLSL(
Texture2D<float4>   g_CurrentFrame   : register(t0);
Texture2D<float4>   g_PrevFrame      : register(t1);
Texture2D<float2>   g_MotionVectors  : register(t2);
RWTexture2D<float>  g_Confidence     : register(u0);

cbuffer ConfParams : register(b0)
{
    float2 g_frameSize;
    float2 g_pad;
};

[numthreads(32, 18, 1)]
void CSPhotoConfidence(uint3 dtid : SV_DispatchThreadID)
{
    int w = (int)g_frameSize.x;
    int h = (int)g_frameSize.y;
    int2 maxCoord = int2(w - 1, h - 1);

    int cellW = max(1, w / 32);
    int cellH = max(1, h / 18);
    int2 pos = clamp(int2((int)dtid.x * cellW + cellW / 2, (int)dtid.y * cellH + cellH / 2), int2(0, 0), maxCoord);

    float2 mv = g_MotionVectors[pos];
    int2 refPos = clamp(pos + int2(round(mv.x), round(mv.y)), int2(0, 0), maxCoord);

    float maxErr = 0.0f;
    [unroll]
    for (int dy = -1; dy <= 1; ++dy)
    {
        [unroll]
        for (int dx = -1; dx <= 1; ++dx)
        {
            int2 cp = clamp(pos    + int2(dx, dy), int2(0, 0), maxCoord);
            int2 rp = clamp(refPos + int2(dx, dy), int2(0, 0), maxCoord);
            float3 diff = g_CurrentFrame[cp].rgb - g_PrevFrame[rp].rgb;
            maxErr = max(maxErr, dot(diff, diff));
        }
    }

    g_Confidence[dtid.xy] = sqrt(maxErr);
}
)HLSL";

bool MotionVectorManager::Init(ID3D11Device* device, int width, int height)
{
    m_width  = width;
    m_height = height;

    if (!CreateResources(device, width, height))       return false;
    if (!CompileShaders(device))                       return false;
    if (!CompilePhotoConfidenceShader(device))          return false;

    // Linear clamp sampler
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter   = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD   = D3D11_FLOAT32_MAX;
    device->CreateSamplerState(&sd, &m_linearSampler);

    // Initialize Hardware NVIDIA Optical Flow (NVOF) engine
    m_nvof = std::make_unique<NvOFManager>();
    ComPtr<ID3D11DeviceContext> immCtx;
    device->GetImmediateContext(&immCtx);

    const auto& cfg = ConfigManager::Get().Config();
    static const NV_OF_PERF_LEVEL kPerfLevelByQuality[3] = {
        NV_OF_PERF_LEVEL_FAST, NV_OF_PERF_LEVEL_MEDIUM, NV_OF_PERF_LEVEL_SLOW
    };
    int qualityIdx = (cfg.nrFlowQuality >= 0 && cfg.nrFlowQuality <= 2) ? cfg.nrFlowQuality : 0;
    NV_OF_PERF_LEVEL perfLevel = kPerfLevelByQuality[qualityIdx];

    if (m_nvof->Init(device, immCtx.Get(), width, height, cfg.nrFlowGrid, perfLevel))
    {
        m_useHardwareNvOF = true;
        DLSS_Log("[OpticalFlow] Hardware NVIDIA Optical Flow (NVOF) active! GridSize=%d, PerfLevel=%d, Zero SM load.",
            cfg.nrFlowGrid, (int)perfLevel);
    }
    else
    {
        m_useHardwareNvOF = false;
        DLSS_Log("[OpticalFlow] Hardware NVOF unavailable or failed; using 8x8 groupshared compute shader fallback.");
    }

    return true;
}

// ---------------------------------------------------------------------------
// Resize
// ---------------------------------------------------------------------------
void MotionVectorManager::Resize(ID3D11Device* device, int width, int height)
{
    if (m_width == width && m_height == height) return;
    m_width  = width;
    m_height = height;
    m_firstFrame = true;

    CreateResources(device, width, height);

    if (m_nvof && m_useHardwareNvOF)
    {
        ComPtr<ID3D11DeviceContext> immCtx;
        device->GetImmediateContext(&immCtx);
        m_nvof->Resize(device, immCtx.Get(), width, height);
    }
}

// ---------------------------------------------------------------------------
// CleanupTextures — only releases textures, preserving compute shader and samplers
// ---------------------------------------------------------------------------
void MotionVectorManager::CleanupTextures()
{
    m_prevFrameSRV.Reset();
    m_prevFrameTexture.Reset();

    m_mvUAV.Reset();
    m_mvSRV.Reset();
    m_mvTexture.Reset();

    m_maskUAV.Reset();
    m_maskSRV.Reset();
    m_maskTexture.Reset();

    m_depthSRV.Reset();
    m_depthTexture.Reset();

    for (int i = 0; i < kProbeRingSize; ++i)
        m_stagingMvRing[i].Reset();
    m_probeRingIndex = 0;
    m_probeFramesPending = 0;

    m_confUAV.Reset();
    m_confTexture.Reset();
    for (int i = 0; i < kConfRingSize; ++i)
        m_confStagingRing[i].Reset();
    m_confRingIndex = 0;
    m_confFramesPending = 0;

    // Harici MV dokusu (D3D12Interop'un paylasimli dokusu) Resize/Init'te
    // degisebilir; eskiye bagli SRV'i dusur, bir sonraki UpdatePhotoConfidence
    // cagrisi tazesiyle yeniden olustursun.
    m_externalMvSRV.Reset();
    m_cachedExternalMvTexPtr = nullptr;
    m_flowSuspect = false;
    m_lastPhotoConfErr = 0.0f;
}

// ---------------------------------------------------------------------------
// Cleanup
// ---------------------------------------------------------------------------
void MotionVectorManager::Cleanup()
{
    CleanupTextures();

    if (m_nvof)
    {
        m_nvof->Cleanup();
        m_nvof.reset();
    }
    m_useHardwareNvOF = false;

    // NOT: s_opticalFlowCS/s_photoConfCS KASITLI OLARAK sifirlanmiyor -- static,
    // process-omurlu (bkz. header aciklamasi). Sadece bu ornege ait (session'a
    // ozel, ucuz) sabit tampon burada dusuruluyor.
    m_photoConfCB.Reset();
    m_constantBuffer.Reset();
    m_linearSampler.Reset();
}

// ---------------------------------------------------------------------------
// CreateResources
// ---------------------------------------------------------------------------
bool MotionVectorManager::CreateResources(ID3D11Device* device, int width, int height)
{
    CleanupTextures();

    // 1. Previous frame cache texture (B8G8R8A8_UNORM)
    D3D11_TEXTURE2D_DESC td = {};
    td.Width          = static_cast<UINT>(width);
    td.Height         = static_cast<UINT>(height);
    td.MipLevels      = 1;
    td.ArraySize      = 1;
    td.Format         = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc     = { 1, 0 };
    td.Usage          = D3D11_USAGE_DEFAULT;
    td.BindFlags      = D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = device->CreateTexture2D(&td, nullptr, &m_prevFrameTexture);
    if (FAILED(hr)) return false;

    hr = device->CreateShaderResourceView(m_prevFrameTexture.Get(), nullptr, &m_prevFrameSRV);
    if (FAILED(hr)) return false;

    // 2. Motion Vectors texture (R16G16_FLOAT) - UAV + SRV
    td.Format    = DXGI_FORMAT_R16G16_FLOAT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

    hr = device->CreateTexture2D(&td, nullptr, &m_mvTexture);
    if (FAILED(hr)) return false;

    hr = device->CreateShaderResourceView(m_mvTexture.Get(), nullptr, &m_mvSRV);
    if (FAILED(hr)) return false;

    hr = device->CreateUnorderedAccessView(m_mvTexture.Get(), nullptr, &m_mvUAV);
    if (FAILED(hr)) return false;

    // 3. Dynamic Reactive UI Mask texture (R8_UNORM) - UAV + SRV
    td.Format    = DXGI_FORMAT_R8_UNORM;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

    hr = device->CreateTexture2D(&td, nullptr, &m_maskTexture);
    if (FAILED(hr)) return false;

    hr = device->CreateShaderResourceView(m_maskTexture.Get(), nullptr, &m_maskSRV);
    if (FAILED(hr)) return false;

    hr = device->CreateUnorderedAccessView(m_maskTexture.Get(), nullptr, &m_maskUAV);
    if (FAILED(hr)) return false;

    // 4. Flat Depth texture (R32_FLOAT) for DLSS
    td.Format    = DXGI_FORMAT_R32_FLOAT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    std::vector<float> zeroDepth(width * height, 0.0f);
    D3D11_SUBRESOURCE_DATA depthInit = {};
    depthInit.pSysMem     = zeroDepth.data();
    depthInit.SysMemPitch = width * sizeof(float);

    hr = device->CreateTexture2D(&td, &depthInit, &m_depthTexture);
    if (FAILED(hr)) return false;

    hr = device->CreateShaderResourceView(m_depthTexture.Get(), nullptr, &m_depthSRV);
    if (FAILED(hr)) return false;

    // 5. Staging texture ring (32x18 R16G16_FLOAT sparse grid) for stall-free periodic diagnostic probing
    D3D11_TEXTURE2D_DESC tdStaging = {};
    tdStaging.Width          = kProbeGridW;
    tdStaging.Height         = kProbeGridH;
    tdStaging.MipLevels      = 1;
    tdStaging.ArraySize      = 1;
    tdStaging.Format         = DXGI_FORMAT_R16G16_FLOAT;
    tdStaging.SampleDesc     = { 1, 0 };
    tdStaging.Usage          = D3D11_USAGE_STAGING;
    tdStaging.BindFlags      = 0;
    tdStaging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    for (int i = 0; i < kProbeRingSize; ++i)
    {
        hr = device->CreateTexture2D(&tdStaging, nullptr, &m_stagingMvRing[i]);
        if (FAILED(hr)) return false;
    }
    m_probeRingIndex = 0;
    m_probeFramesPending = 0;

    // 5b. Fotometrik guven ciktisi (32x18 R32_FLOAT UAV) + async okuma icin ring.
    // Probe'un aksine HER karede yazilir (sahne kesmesi herhangi bir karede olabilir).
    D3D11_TEXTURE2D_DESC tdConf = {};
    tdConf.Width          = kProbeGridW;
    tdConf.Height         = kProbeGridH;
    tdConf.MipLevels      = 1;
    tdConf.ArraySize      = 1;
    tdConf.Format         = DXGI_FORMAT_R32_FLOAT;
    tdConf.SampleDesc     = { 1, 0 };
    tdConf.Usage          = D3D11_USAGE_DEFAULT;
    tdConf.BindFlags      = D3D11_BIND_UNORDERED_ACCESS;

    hr = device->CreateTexture2D(&tdConf, nullptr, &m_confTexture);
    if (FAILED(hr)) return false;

    hr = device->CreateUnorderedAccessView(m_confTexture.Get(), nullptr, &m_confUAV);
    if (FAILED(hr)) return false;

    D3D11_TEXTURE2D_DESC tdConfStaging = tdConf;
    tdConfStaging.Usage          = D3D11_USAGE_STAGING;
    tdConfStaging.BindFlags      = 0;
    tdConfStaging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    for (int i = 0; i < kConfRingSize; ++i)
    {
        hr = device->CreateTexture2D(&tdConfStaging, nullptr, &m_confStagingRing[i]);
        if (FAILED(hr)) return false;
    }
    m_confRingIndex = 0;
    m_confFramesPending = 0;
    m_externalMvSRV.Reset();
    m_cachedExternalMvTexPtr = nullptr;
    m_flowSuspect = false;
    m_lastPhotoConfErr = 0.0f;

    // 6. Constant buffer for Compute Shader (create if not already existing)
    if (!m_constantBuffer)
    {
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth      = sizeof(ComputeCB);
        bd.Usage          = D3D11_USAGE_DEFAULT;
        bd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;

        hr = device->CreateBuffer(&bd, nullptr, &m_constantBuffer);
        if (FAILED(hr)) return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// CompileShaders
// ---------------------------------------------------------------------------
bool MotionVectorManager::CompileShaders(ID3D11Device* device)
{
    if (s_opticalFlowCS) return true;

    ComPtr<ID3DBlob> blob, errBlob;

    HRESULT hr = D3DCompile(
        kOpticalFlowCSSource, strlen(kOpticalFlowCSSource),
        "OpticalFlowCS", nullptr, nullptr,
        "CSMain", "cs_5_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
        &blob, &errBlob);

    if (FAILED(hr))
    {
        if (errBlob)
            OutputDebugStringA(static_cast<char*>(errBlob->GetBufferPointer()));
        return false;
    }

    hr = device->CreateComputeShader(
        blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &s_opticalFlowCS);

    return SUCCEEDED(hr);
}

// ---------------------------------------------------------------------------
// CompilePhotoConfidenceShader
// ---------------------------------------------------------------------------
bool MotionVectorManager::CompilePhotoConfidenceShader(ID3D11Device* device)
{
    if (!s_photoConfCS)
    {
        ComPtr<ID3DBlob> blob, errBlob;
        HRESULT hr = D3DCompile(
            kPhotoConfCSSource, strlen(kPhotoConfCSSource),
            "PhotoConfidenceCS", nullptr, nullptr,
            "CSPhotoConfidence", "cs_5_0",
            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
            &blob, &errBlob);

        if (FAILED(hr))
        {
            if (errBlob)
                OutputDebugStringA(static_cast<char*>(errBlob->GetBufferPointer()));
            return false;
        }

        hr = device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &s_photoConfCS);
        if (FAILED(hr)) return false;
    }

    if (!m_photoConfCB)
    {
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = 16; // float2 g_frameSize + float2 padding, 16-byte cbuffer aligned
        bd.Usage     = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        HRESULT hr = device->CreateBuffer(&bd, nullptr, &m_photoConfCB);
        if (FAILED(hr)) return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// IEEE 754 half-precision float converter for MV probe
// ---------------------------------------------------------------------------
static inline float HalfToFloat(uint16_t h)
{
    uint32_t s = (h >> 15) & 0x0001;
    uint32_t e = (h >> 10) & 0x001f;
    uint32_t m =  h        & 0x03ff;

    if (e == 0)
    {
        if (m == 0) return s ? -0.0f : 0.0f;
        while ((m & 0x0400) == 0) { m <<= 1; e--; }
        e++;
        m &= ~0x0400;
    }
    else if (e == 31)
    {
        return 0.0f;
    }

    e = e + (127 - 15);
    m = m << 13;
    uint32_t u = (s << 31) | (e << 23) | m;
    float f;
    memcpy(&f, &u, sizeof(float));
    return f;
}

// ---------------------------------------------------------------------------
// ProbeMotionVectors — periodic CPU probe of a sparse grid spanning the whole
// frame (kProbeGridW x kProbeGridH points, one per cell). A single center
// block only ever reported ONE motion direction, so mean and max were always
// identical and said nothing about the flow field elsewhere on screen. Still
// periodic (see s_probeCounter cadence below) -- this never runs per-frame.
// ---------------------------------------------------------------------------
void MotionVectorManager::ProbeMotionVectors(ID3D11DeviceContext* ctx, ID3D11Texture2D* mvTex)
{
    if (!ctx || !mvTex || !m_stagingMvRing[0]) return;

    static uint32_t s_probeCounter = 0;
    s_probeCounter++;

    bool shouldProbe = (s_probeCounter <= 5 || (s_probeCounter % 300 == 0));
    if (shouldProbe)
    {
        // Her hucrenin merkezinden TEK piksel kopyala: 576 kucuk CopySubresourceRegion,
        // hepsi tek bir izgara dokusuna -- tek Map/Unmap yeterli. Bu sadece nadiren
        // (baslangicta 5 kare + her 300 karede bir) calisir, per-frame maliyeti yok.
        int writeSlot = m_probeRingIndex;
        ID3D11Texture2D* dst = m_stagingMvRing[writeSlot].Get();

        for (int gy = 0; gy < kProbeGridH; ++gy)
        {
            int sy = std::min(m_height - 1, (gy * m_height) / kProbeGridH + (m_height / kProbeGridH) / 2);
            for (int gx = 0; gx < kProbeGridW; ++gx)
            {
                int sx = std::min(m_width - 1, (gx * m_width) / kProbeGridW + (m_width / kProbeGridW) / 2);

                D3D11_BOX box = {};
                box.left   = static_cast<UINT>(sx);
                box.top    = static_cast<UINT>(sy);
                box.front  = 0;
                box.right  = static_cast<UINT>(sx + 1);
                box.bottom = static_cast<UINT>(sy + 1);
                box.back   = 1;

                ctx->CopySubresourceRegion(dst, 0, static_cast<UINT>(gx), static_cast<UINT>(gy), 0, mvTex, 0, &box);
            }
        }

        m_probeRingIndex = (m_probeRingIndex + 1) % kProbeRingSize;
        m_probeFramesPending++;

        // Only read from the ring buffer if enough frames have elapsed so GPU has finished execution,
        // completely eliminating CPU-GPU pipeline flush stalls.
        if (m_probeFramesPending >= kProbeRingSize)
        {
            int readSlot = m_probeRingIndex; // Oldest slot in the ring (kProbeRingSize frames old)
            D3D11_MAPPED_SUBRESOURCE mapped = {};
            if (SUCCEEDED(ctx->Map(m_stagingMvRing[readSlot].Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped)))
            {
                const uint8_t* pBase = reinterpret_cast<const uint8_t*>(mapped.pData);

                float sampleMag[kProbeGridW * kProbeGridH];
                int nonZeroCount = 0;
                int sampleCount = 0;

                for (int y = 0; y < kProbeGridH; ++y)
                {
                    const uint16_t* pRow = reinterpret_cast<const uint16_t*>(pBase + y * mapped.RowPitch);
                    for (int x = 0; x < kProbeGridW; ++x)
                    {
                        float dx = HalfToFloat(pRow[x * 2 + 0]);
                        float dy = HalfToFloat(pRow[x * 2 + 1]);
                        float mag = std::sqrt(dx * dx + dy * dy);
                        if (mag > 0.01f) nonZeroCount++;
                        sampleMag[sampleCount++] = mag;
                    }
                }
                ctx->Unmap(m_stagingMvRing[readSlot].Get(), 0);

                std::sort(sampleMag, sampleMag + sampleCount);
                float p50 = sampleMag[(sampleCount * 50) / 100];
                float p95 = sampleMag[(sampleCount * 95) / 100];
                float maxMag = sampleMag[sampleCount - 1];
                float nonZeroPct = (nonZeroCount * 100.0f) / (float)sampleCount;

                DLSS_Log("[OpticalFlow] Probe #%u (%dx%d, grid=%dx%d): p50 |mv|=%.2f px, p95=%.2f px, max=%.2f px, nonZero=%.1f%%",
                    s_probeCounter, m_width, m_height, kProbeGridW, kProbeGridH, p50, p95, maxMag, nonZeroPct);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// UpdatePhotoConfidence — HER karede calisir (Probe'un aksine periyodik degil,
// sahne kesmesi herhangi bir karede olabilir). m_prevFrameSRV bu karenin
// rengiyle EZILMEDEN once cagrilmali (bkz. ProcessFrame cagri yeri).
// ---------------------------------------------------------------------------
void MotionVectorManager::UpdatePhotoConfidence(
    ID3D11DeviceContext* ctx,
    ID3D11ShaderResourceView* currentFrameSRV,
    ID3D11Texture2D* mvTex)
{
    if (!ctx || !currentFrameSRV || !mvTex || !s_photoConfCS || !m_photoConfCB || !m_confUAV || !m_prevFrameSRV)
        return;

    // Hardware NvOF yolunda hareket vektorleri D3D12Interop'un paylasimli
    // dokusuna yaziliyor (mvTex != m_mvTexture); o dokunun SRV'si bize
    // gecilmiyor, pointer degismedigi surece burada bir kez olusturup
    // onbellekliyoruz.
    ID3D11ShaderResourceView* mvSRV = nullptr;
    if (mvTex == m_mvTexture.Get())
    {
        mvSRV = m_mvSRV.Get();
    }
    else
    {
        if (mvTex != m_cachedExternalMvTexPtr)
        {
            m_externalMvSRV.Reset();
            m_cachedExternalMvTexPtr = nullptr;
            ComPtr<ID3D11Device> device;
            ctx->GetDevice(&device);
            if (device && SUCCEEDED(device->CreateShaderResourceView(mvTex, nullptr, &m_externalMvSRV)))
            {
                m_cachedExternalMvTexPtr = mvTex;
            }
        }
        mvSRV = m_externalMvSRV.Get();
    }
    if (!mvSRV) return;

    struct { float w, h, pad0, pad1; } cb = { (float)m_width, (float)m_height, 0.0f, 0.0f };
    ctx->UpdateSubresource(m_photoConfCB.Get(), 0, nullptr, &cb, 0, 0);

    ctx->CSSetShader(s_photoConfCS.Get(), nullptr, 0);
    ctx->CSSetConstantBuffers(0, 1, m_photoConfCB.GetAddressOf());

    ID3D11ShaderResourceView* srvs[3] = { currentFrameSRV, m_prevFrameSRV.Get(), mvSRV };
    ctx->CSSetShaderResources(0, 3, srvs);

    ID3D11UnorderedAccessView* uavs[1] = { m_confUAV.Get() };
    UINT initCounts[1] = { 0 };
    ctx->CSSetUnorderedAccessViews(0, 1, uavs, initCounts);

    ctx->Dispatch(1, 1, 1); // numthreads(32,18,1) == tum izgara tek grupta

    ID3D11UnorderedAccessView* nullUAVs[1] = { nullptr };
    ctx->CSSetUnorderedAccessViews(0, 1, nullUAVs, nullptr);
    ID3D11ShaderResourceView* nullSRVs[3] = { nullptr, nullptr, nullptr };
    ctx->CSSetShaderResources(0, 3, nullSRVs);

    // Async, stall-free readback: her karede yaz, kConfRingSize kare once
    // yazilan (hazir olmasi garanti) slotu DO_NOT_WAIT ile oku. Hazir
    // degilse bu karede m_flowSuspect eski degerinde kalir -- pipeline
    // hicbir zaman durmaz (Probe'daki ayni desen).
    int writeSlot = m_confRingIndex;
    ctx->CopyResource(m_confStagingRing[writeSlot].Get(), m_confTexture.Get());
    m_confRingIndex = (m_confRingIndex + 1) % kConfRingSize;
    m_confFramesPending++;

    if (m_confFramesPending >= kConfRingSize)
    {
        int readSlot = m_confRingIndex;
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (SUCCEEDED(ctx->Map(m_confStagingRing[readSlot].Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped)))
        {
            const uint8_t* pBase = reinterpret_cast<const uint8_t*>(mapped.pData);
            const int count = kProbeGridW * kProbeGridH;
            float sum = 0.0f;
            for (int y = 0; y < kProbeGridH; ++y)
            {
                const float* pRow = reinterpret_cast<const float*>(pBase + y * mapped.RowPitch);
                for (int x = 0; x < kProbeGridW; ++x)
                    sum += pRow[x];
            }
            ctx->Unmap(m_confStagingRing[readSlot].Get(), 0);

            // Ampirik baslangic esigi (0..~1.73 RGB fark buyuklugu araliginda);
            // olculmeden degistirme -- Gorev 3'teki NrFlowQuality gibi.
            static constexpr float kPhotoConfSuspectThreshold = 0.35f;

            m_lastPhotoConfErr = sum / (float)count;
            bool wasSuspect = m_flowSuspect;
            m_flowSuspect = (m_lastPhotoConfErr > kPhotoConfSuspectThreshold);

            if (m_flowSuspect && !wasSuspect)
            {
                DLSS_Log("[OpticalFlow] Fotometrik guven dustu: ort hata=%.3f (esik=%.2f) -> akis supheli, sonraki DLSS-NR degerlendirmesinde reset zorlanacak",
                    m_lastPhotoConfErr, kPhotoConfSuspectThreshold);
            }
            else if (!m_flowSuspect && wasSuspect)
            {
                DLSS_Log("[OpticalFlow] Fotometrik guven toparlandi: ort hata=%.3f (esik=%.2f)",
                    m_lastPhotoConfErr, kPhotoConfSuspectThreshold);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// ProcessFrame
// ---------------------------------------------------------------------------
bool MotionVectorManager::ProcessFrame(
    ID3D11DeviceContext* ctx,
    ID3D11ShaderResourceView* currentFrameSRV,
    ID3D11Texture2D* currentFrameTex,
    ID3D11UnorderedAccessView* targetMvUAV,
    ID3D11Texture2D* targetMvTex)
{
    if (!ctx || !currentFrameSRV || !s_opticalFlowCS) return false;

    ID3D11UnorderedAccessView* activeMvUAV = targetMvUAV ? targetMvUAV : m_mvUAV.Get();
    if (!activeMvUAV) return false;

    // Extract underlying texture from current SRV if not explicitly passed
    ComPtr<ID3D11Texture2D> currentTexture = currentFrameTex;
    if (!currentTexture)
    {
        ComPtr<ID3D11Resource> res;
        currentFrameSRV->GetResource(&res);
        if (FAILED(res.As(&currentTexture)) || !currentTexture) return false;
    }

    if (m_firstFrame)
    {
        // First frame: copy to previous and initialize zero motion vectors
        if (m_prevFrameTexture)
            ctx->CopyResource(m_prevFrameTexture.Get(), currentTexture.Get());

        float zeroColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        ctx->ClearUnorderedAccessViewFloat(activeMvUAV, zeroColor);
        if (m_maskUAV) ctx->ClearUnorderedAccessViewFloat(m_maskUAV.Get(), zeroColor);
        m_firstFrame = false;
        return true;
    }

    // If hardware NVIDIA Optical Flow is active, execute on dedicated silicon with ZERO SM utilization
    bool nvofExecuted = false;
    if (m_useHardwareNvOF && m_nvof)
    {
        nvofExecuted = m_nvof->ProcessFrame(ctx, currentTexture.Get(), m_prevFrameTexture.Get(), activeMvUAV, targetMvTex);
    }

    // Fallback: If hardware NVOF is not available or failed on this frame, use 8x8 groupshared compute shader
    if (!nvofExecuted)
    {
        // Update constant buffer
        ComputeCB cb = {};
        cb.screenSize[0]     = static_cast<float>(m_width);
        cb.screenSize[1]     = static_cast<float>(m_height);
        cb.invScreenSize[0]  = 1.0f / cb.screenSize[0];
        cb.invScreenSize[1]  = 1.0f / cb.screenSize[1];
        cb.uiMaskEnabled     = m_uiMaskEnabled ? 1 : 0;
        cb.uiMaskThreshold   = m_uiMaskThreshold;

        ctx->UpdateSubresource(m_constantBuffer.Get(), 0, nullptr, &cb, 0, 0);

        // Bind resources to compute shader
        ctx->CSSetShader(s_opticalFlowCS.Get(), nullptr, 0);
        ctx->CSSetConstantBuffers(0, 1, m_constantBuffer.GetAddressOf());
        ctx->CSSetSamplers(0, 1, m_linearSampler.GetAddressOf());

        ID3D11ShaderResourceView* srvs[2] = { currentFrameSRV, m_prevFrameSRV.Get() };
        ctx->CSSetShaderResources(0, 2, srvs);

        ID3D11UnorderedAccessView* uavs[2] = { activeMvUAV, m_maskUAV.Get() };
        UINT initCounts[2] = { 0, 0 };
        ctx->CSSetUnorderedAccessViews(0, 2, uavs, initCounts);

        // Dispatch compute shader (8x8 thread blocks)
        UINT groupsX = (static_cast<UINT>(m_width)  + 7) / 8;
        UINT groupsY = (static_cast<UINT>(m_height) + 7) / 8;
        ctx->Dispatch(groupsX, groupsY, 1);

        // Unbind UAVs and SRVs to avoid hazard in downstream rendering
        ID3D11UnorderedAccessView* nullUAVs[2] = { nullptr, nullptr };
        ctx->CSSetUnorderedAccessViews(0, 2, nullUAVs, nullptr);

        ID3D11ShaderResourceView* nullSRVs[2] = { nullptr, nullptr };
        ctx->CSSetShaderResources(0, 2, nullSRVs);
    }

    // Periodic diagnostic probe of motion vectors
    ID3D11Texture2D* activeMvTex = targetMvTex ? targetMvTex : m_mvTexture.Get();
    ProbeMotionVectors(ctx, activeMvTex);

    // Fotometrik kare guveni: m_prevFrameSRV bu karenin rengiyle EZILMEDEN
    // once cagrilmali, o yuzden asagidaki CopyResource'tan ONCE.
    UpdatePhotoConfidence(ctx, currentFrameSRV, activeMvTex);

    // Cache current frame for next iteration
    if (m_prevFrameTexture)
        ctx->CopyResource(m_prevFrameTexture.Get(), currentTexture.Get());

    return true;
}
