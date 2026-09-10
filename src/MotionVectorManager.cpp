#include "MotionVectorManager.h"

// ---------------------------------------------------------------------------
// Compute Shader for Optical Flow & Dynamic UI Reactive Mask
// ---------------------------------------------------------------------------
static const char* s_opticalFlowCS = R"HLSL(
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

[numthreads(16, 16, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    int2 pos = int2(dispatchThreadId.xy);
    if (pos.x >= (int)g_screenSize.x || pos.y >= (int)g_screenSize.y)
        return;

    float3 curCenter  = g_CurrentFrame[pos].rgb;
    float3 prevCenter = g_PrevFrame[pos].rgb;

    // Fast static check: if pixel barely changed between frames, motion is strictly 0.
    // Modern games have sub-pixel jitter / shadow dithering / AO noise of ~2-4 color levels.
    float3 centerDiff = curCenter - prevCenter;
    float centerErr = dot(centerDiff, centerDiff);
    if (centerErr < 0.0003f)
    {
        g_MotionVectors[pos] = float2(0.0f, 0.0f);
        if (g_uiMaskEnabled != 0) g_ReactiveMask[pos] = 1.0f;
        return;
    }

    // Deep shadow check: in flat, dark shadow regions (< 0.04 luminance), visual tracking is
    // ill-posed (aperture problem). Any subtle temporal noise in shadows causes random false vectors.
    float maxLum = max(curCenter.r, max(curCenter.g, curCenter.b));
    if (maxLum < 0.04f && centerErr < 0.003f)
    {
        g_MotionVectors[pos] = float2(0.0f, 0.0f);
        if (g_uiMaskEnabled != 0) g_ReactiveMask[pos] = 1.0f;
        return;
    }

    // 1. Hierarchical Block Search for Optical Flow Motion Vectors
    // Search window: [-12, 12] in pixels
    // Require candidate to beat centerErr by at least 20% and pay distance penalty to prevent jumping.
    int2 bestOffset = int2(0, 0);
    float minError  = centerErr * 0.80f;

    // Coarse search: stride 3 (-12 to 12)
    [unroll]
    for (int dy = -12; dy <= 12; dy += 3)
    {
        for (int dx = -12; dx <= 12; dx += 3)
        {
            if (dx == 0 && dy == 0) continue;
            int2 samplePos = clamp(pos + int2(dx, dy), int2(0, 0), int2((int)g_screenSize.x - 1, (int)g_screenSize.y - 1));
            float3 prevSample = g_PrevFrame[samplePos].rgb;
            float3 diff = curCenter - prevSample;
            float err = dot(diff, diff) + 0.00012f * (float)(abs(dx) + abs(dy));
            if (err < minError)
            {
                minError   = err;
                bestOffset = int2(dx, dy);
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
                int2 samplePos = clamp(pos + candidate, int2(0, 0), int2((int)g_screenSize.x - 1, (int)g_screenSize.y - 1));
                float3 prevSample = g_PrevFrame[samplePos].rgb;
                float3 diff = curCenter - prevSample;
                float err = dot(diff, diff) + 0.00012f * (float)(abs(candidate.x) + abs(candidate.y));
                if (err < minError)
                {
                    minError   = err;
                    bestOffset = candidate;
                }
            }
        }
    }

    // Motion vector in pixels (points from current pixel to previous pixel position)
    float2 mv = float2(bestOffset);

    // 2. Dynamic UI / Disocclusion Reactive Mask
    float mask = 0.0f;
    if (g_uiMaskEnabled != 0)
    {
        float diffSamePos = length(centerDiff);
        float mvLength    = length(mv);

        // If pixel is static UI or no motion detected
        if (diffSamePos < g_uiMaskThreshold || mvLength < 0.25f)
        {
            mask = 1.0f;
            mv   = float2(0.0f, 0.0f);
        }
        else if (minError > 0.35f)
        {
            // Disoccluded area / cutscene: no good match found in previous frame
            mask = 1.0f;
            mv   = float2(0.0f, 0.0f);
        }
        else
        {
            mask = 0.0f;
        }
    }

    g_MotionVectors[pos] = mv;
    g_ReactiveMask[pos]  = mask;
}
)HLSL";

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
bool MotionVectorManager::Init(ID3D11Device* device, int width, int height)
{
    m_width  = width;
    m_height = height;

    if (!CreateResources(device, width, height)) return false;
    if (!CompileShaders(device))                 return false;

    // Linear clamp sampler
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter   = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD   = D3D11_FLOAT32_MAX;
    device->CreateSamplerState(&sd, &m_linearSampler);

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

    m_stagingMv.Reset();
}

// ---------------------------------------------------------------------------
// Cleanup
// ---------------------------------------------------------------------------
void MotionVectorManager::Cleanup()
{
    CleanupTextures();

    m_opticalFlowCS.Reset();
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

    // 5. Staging texture (16x16 R16G16_FLOAT) for periodic diagnostic probing
    D3D11_TEXTURE2D_DESC tdStaging = {};
    tdStaging.Width          = 16;
    tdStaging.Height         = 16;
    tdStaging.MipLevels      = 1;
    tdStaging.ArraySize      = 1;
    tdStaging.Format         = DXGI_FORMAT_R16G16_FLOAT;
    tdStaging.SampleDesc     = { 1, 0 };
    tdStaging.Usage          = D3D11_USAGE_STAGING;
    tdStaging.BindFlags      = 0;
    tdStaging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    device->CreateTexture2D(&tdStaging, nullptr, &m_stagingMv);

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
    ComPtr<ID3DBlob> blob, errBlob;

    HRESULT hr = D3DCompile(
        s_opticalFlowCS, strlen(s_opticalFlowCS),
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
        blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &m_opticalFlowCS);

    return SUCCEEDED(hr);
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
// ProbeMotionVectors — periodic CPU probe of center 16x16 motion vectors
// ---------------------------------------------------------------------------
void MotionVectorManager::ProbeMotionVectors(ID3D11DeviceContext* ctx, ID3D11Texture2D* mvTex)
{
    if (!ctx || !mvTex || !m_stagingMv) return;

    static uint32_t s_probeCounter = 0;
    s_probeCounter++;
    if (s_probeCounter <= 5 || (s_probeCounter % 300 == 0))
    {
        int cx = std::max(0, m_width / 2 - 8);
        int cy = std::max(0, m_height / 2 - 8);
        D3D11_BOX box = {};
        box.left   = static_cast<UINT>(cx);
        box.top    = static_cast<UINT>(cy);
        box.front  = 0;
        box.right  = static_cast<UINT>(cx + 16);
        box.bottom = static_cast<UINT>(cy + 16);
        box.back   = 1;

        ctx->CopySubresourceRegion(m_stagingMv.Get(), 0, 0, 0, 0, mvTex, 0, &box);

        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (SUCCEEDED(ctx->Map(m_stagingMv.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
        {
            const uint16_t* pData = reinterpret_cast<const uint16_t*>(mapped.pData);
            UINT pitchElements = mapped.RowPitch / sizeof(uint16_t);

            float sumMag = 0.0f;
            float maxMag = 0.0f;
            int nonZeroCount = 0;
            float centerDx = 0.0f, centerDy = 0.0f;

            for (int y = 0; y < 16; ++y)
            {
                for (int x = 0; x < 16; ++x)
                {
                    uint16_t hx = pData[y * pitchElements + x * 2 + 0];
                    uint16_t hy = pData[y * pitchElements + x * 2 + 1];
                    float dx = HalfToFloat(hx);
                    float dy = HalfToFloat(hy);
                    float mag = std::sqrt(dx * dx + dy * dy);
                    if (mag > 0.01f) nonZeroCount++;
                    sumMag += mag;
                    if (mag > maxMag) maxMag = mag;
                    if (x == 8 && y == 8) { centerDx = dx; centerDy = dy; }
                }
            }
            ctx->Unmap(m_stagingMv.Get(), 0);

            float meanMag = sumMag / 256.0f;
            float nonZeroPct = (nonZeroCount * 100.0f) / 256.0f;
            DLSS_Log("[OpticalFlow] Probe #%u (%dx%d): mean |mv|=%.2f px, max=%.2f px, nonZero=%.1f%%, center=(%.1f, %.1f) px",
                s_probeCounter, m_width, m_height, meanMag, maxMag, nonZeroPct, centerDx, centerDy);
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
    if (!ctx || !currentFrameSRV || !m_opticalFlowCS) return false;

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
    ctx->CSSetShader(m_opticalFlowCS.Get(), nullptr, 0);
    ctx->CSSetConstantBuffers(0, 1, m_constantBuffer.GetAddressOf());
    ctx->CSSetSamplers(0, 1, m_linearSampler.GetAddressOf());

    ID3D11ShaderResourceView* srvs[2] = { currentFrameSRV, m_prevFrameSRV.Get() };
    ctx->CSSetShaderResources(0, 2, srvs);

    ID3D11UnorderedAccessView* uavs[2] = { activeMvUAV, m_maskUAV.Get() };
    UINT initCounts[2] = { 0, 0 };
    ctx->CSSetUnorderedAccessViews(0, 2, uavs, initCounts);

    // Dispatch compute shader (16x16 thread blocks)
    UINT groupsX = (static_cast<UINT>(m_width)  + 15) / 16;
    UINT groupsY = (static_cast<UINT>(m_height) + 15) / 16;
    ctx->Dispatch(groupsX, groupsY, 1);

    // Unbind UAVs and SRVs to avoid hazard in downstream rendering
    ID3D11UnorderedAccessView* nullUAVs[2] = { nullptr, nullptr };
    ctx->CSSetUnorderedAccessViews(0, 2, nullUAVs, nullptr);

    ID3D11ShaderResourceView* nullSRVs[2] = { nullptr, nullptr };
    ctx->CSSetShaderResources(0, 2, nullSRVs);

    // Periodic diagnostic probe of motion vectors
    ID3D11Texture2D* activeMvTex = targetMvTex ? targetMvTex : m_mvTexture.Get();
    ProbeMotionVectors(ctx, activeMvTex);

    // Cache current frame for next iteration
    if (m_prevFrameTexture)
        ctx->CopyResource(m_prevFrameTexture.Get(), currentTexture.Get());

    return true;
}
