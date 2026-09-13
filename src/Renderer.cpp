#include "Renderer.h"
#include "ConfigManager.h"

// -----------------------------------------------------------------------
// HLSL shaders (compiled at runtime via D3DCompile)
// Full-screen triangle approach: no vertex buffer needed.
// -----------------------------------------------------------------------
static const char* s_vsSource = R"HLSL(
void VS(uint   id  : SV_VertexID,
        out float4 pos : SV_Position,
        out float2 uv  : TEXCOORD0)
{
    // Positions and UVs for a CCW full-screen triangle
    float2 verts[3] = {
        float2(-1.0f,  1.0f),
        float2( 3.0f,  1.0f),
        float2(-1.0f, -3.0f)
    };
    float2 uvs[3] = {
        float2(0.0f, 0.0f),
        float2(2.0f, 0.0f),
        float2(0.0f, 2.0f)
    };
    pos = float4(verts[id], 0.0f, 1.0f);
    uv  = uvs[id];
}
)HLSL";

static const char* s_psSource = R"HLSL(
Texture2D<float4> gModelTex    : register(t0);
Texture2D<float4> gProxyTex    : register(t1);
Texture2D<float4> gOriginalTex : register(t2);
Texture2D<float4> gFpsTex      : register(t3);

SamplerState      gPointSamp   : register(s0);
SamplerState      gLinearSamp  : register(s1);

cbuffer FpsConfig : register(b0)
{
    float2 g_screenSize;       // (width, height)
    float2 g_fpsBoxSize;       // (180, 36)
    int    g_fpsEnabled;       // 1 = enabled, 0 = disabled
    float  g_boostFactor;      // 1.0 = normal, >1.0 = linear extrapolation boost
    int    g_dlssnrActive;     // 1 = DLSS-NR active, 0 = passthrough
    float  g_intensity;        // DLSS-NR intensity / detail strength (0.0 - 2.0)
    float  g_colourStrength;   // Colour strength (0.0 - 1.0)
    int    g_isSubNative;      // 1 = model resolution < 100% (Residual active)
    float2 g_workTexelSize;    // 1.0 / workSize
    int    g_splitEnabled;     // 1 = split screen enabled, 0 = disabled
    float  g_splitPos;         // 0.0 to 1.0 (screen x fraction)
    float2 g_padding;          // 16-byte alignment
};

float4 PS(float4 pos : SV_Position,
          float2 uv  : TEXCOORD0) : SV_Target
{
    float4 originalSample = gOriginalTex.Sample(gPointSamp, uv);
    float3 original = originalSample.rgb;
    float3 result = original;

    if (g_dlssnrActive != 0)
    {
        float3 model = gModelTex.Sample(gLinearSamp, uv).rgb;
        float3 raw = gProxyTex.Sample(gLinearSamp, uv).rgb;

        // Boost linear extrapolation: saturate(raw + boost * (model - raw))
        float3 boostedModel = saturate(raw + (model - raw) * g_boostFactor);

        if (g_isSubNative == 0)
        {
            // At 100% resolution: direct, natural DLSS-NR neural output with boost
            result = lerp(original, boostedModel, saturate(g_intensity));
        }
        else
        {
            // At sub-native resolution (50% - 99%):
            // OptiScaler / RenoDX Luminance-Ratio Transfer:
            // High-frequency geometry, edges, HUD, text, and micro-textures come 100%
            // from the native full-resolution frame ('original') without blur.
            // DLSS 5's neural denoising and lighting adjustments are transferred as a
            // smooth luminance ratio. Eliminates phase-mismatch watercolor smearing entirely.
            float3 proxy = raw;

            const float3 kLuma = float3(0.2126f, 0.7152f, 0.0722f);
            float proxyLuma = dot(proxy, kLuma);
            float modelLuma = dot(boostedModel, kLuma);

            // Stabilized lighting ratio with floor to prevent near-black blowing up
            const float kRatioFloor = 1.0f / 512.0f;
            float rawRatio = (modelLuma + kRatioFloor) / (proxyLuma + kRatioFloor);

            // Guard ratio against runaway highlights or extreme darkening
            const float kMaxRatio = 2.0f;
            float boundedRatio = clamp(rawRatio, 1.0f / kMaxRatio, kMaxRatio);

            // Detail strength modulation
            float effectiveRatio = lerp(1.0f, boundedRatio, saturate(g_intensity));

            // Transferred neural lighting on pristine native frame
            float3 transferredLuma = original * effectiveRatio;

            // Chrominance transfer: model's clean neural chroma scaled to match transferred luma
            float targetLuma = dot(transferredLuma, kLuma);
            float3 modelChroma = boostedModel * (targetLuma / max(modelLuma, 1e-5f));

            // Blend between luminance-only transfer and model chroma
            result = lerp(transferredLuma, modelChroma, saturate(g_colourStrength));
        }
    }

    float3 color;
    if (g_splitEnabled != 0)
    {
        float splitCoord = g_splitPos * g_screenSize.x;
        float dist = pos.x - splitCoord;

        // 2-pixel wide crisp accent divider line at the boundary
        if (abs(dist) <= 1.0f)
        {
            color = float3(1.0f, 1.0f, 1.0f);
        }
        else if (dist < 0.0f)
        {
            // Sol taraf: Ham görüntü (hiçbir filtre veya DLSS uygulanmamış orijinal kare)
            color = original;
        }
        else
        {
            // Sağ taraf: DLSS 5 çıktısı
            color = result;
        }
    }
    else
    {
        color = result;
    }

    if (g_fpsEnabled != 0)
    {
        float margin = 16.0f;
        float left   = g_screenSize.x - g_fpsBoxSize.x - margin;
        float right  = g_screenSize.x - margin;
        float top    = margin;
        float bottom = margin + g_fpsBoxSize.y;

        if (pos.x >= left && pos.x < right && pos.y >= top && pos.y < bottom)
        {
            float2 fpsUv = float2((pos.x - left) / g_fpsBoxSize.x, (pos.y - top) / g_fpsBoxSize.y);
            float4 fpsColor = gFpsTex.Sample(gLinearSamp, fpsUv);
            color = fpsColor.rgb * fpsColor.a + color * (1.0f - fpsColor.a);
        }
    }

    return float4(saturate(color), 1.0f);
}
)HLSL";

static constexpr int kFpsWidth  = 180;
static constexpr int kFpsHeight = 36;

struct FpsCBufferData
{
    float screenSize[2];       // offset 0 (8 bytes)
    float fpsBoxSize[2];       // offset 8 (8 bytes) -> 16 bytes
    int   fpsEnabled;          // offset 16 (4 bytes)
    float boostFactor;         // offset 20 (4 bytes)
    int   dlssnrActive;        // offset 24 (4 bytes)
    float intensity;           // offset 28 (4 bytes) -> 32 bytes
    float colourStrength;      // offset 32 (4 bytes)
    int   isSubNative;         // offset 36 (4 bytes)
    float workTexelSize[2];    // offset 40 (8 bytes) -> 48 bytes
    int   splitEnabled;        // offset 48 (4 bytes)
    float splitPos;            // offset 52 (4 bytes)
    float padding[2];          // offset 56 (8 bytes) -> 64 bytes
};

// -----------------------------------------------------------------------
// Init
// -----------------------------------------------------------------------
bool Renderer::Init(ID3D11Device* device, HWND overlayHwnd, int width, int height)
{
    m_width  = width;
    m_height = height;

    // GPU Thread Priority: Boost to maximum (+7) so that VLSS5's pipeline
    // (capture -> downscale -> NR model -> present) receives earlier time slices in WDDM scheduler.
    ComPtr<IDXGIDevice> dxgiDevice;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) && dxgiDevice)
    {
        dxgiDevice->SetGPUThreadPriority(7); // -7..7 range, 7 = maximum
    }

    if (!CreateSwapChain(device, overlayHwnd, width, height)) return false;
    if (!CreateRTV(device))                                    return false;
    if (!CompileShaders(device))                               return false;
    if (!CreateFpsResources(device))                           return false;

    // Point sampler for 1:1 pixel-perfect game capture (no blur / ghosting on text or HUD)
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter   = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD   = D3D11_FLOAT32_MAX;
    device->CreateSamplerState(&sd, &m_sampler);

    // Bilinear sampler for smooth enlargement when model resolution scale < 100%
    sd.Filter   = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    device->CreateSamplerState(&sd, &m_linearSampler);

    ComPtr<ID3D11DeviceContext> ctx;
    device->GetImmediateContext(&ctx);

    // 1. Initialize D3D12Interop & DLSSNRManager (DLSS 5 Neural Rendering) FIRST
    m_d3d12Interop = std::make_unique<D3D12Interop>();
    bool nrInitialized = false;

    const auto& initialCfg = ConfigManager::Get().Config();
    float initScale = initialCfg.resolutionScale / 100.0f;
    if (initScale < 0.50f) initScale = 0.50f;
    if (initScale > 1.00f) initScale = 1.00f;
    int initWorkW = ((int)(width * initScale + 0.5f) + 1) & ~1;
    int initWorkH = ((int)(height * initScale + 0.5f) + 1) & ~1;

    if (m_d3d12Interop->Init(device, ctx.Get(), width, height, initWorkW, initWorkH))
    {
        m_dlssnrManager = std::make_unique<DLSSNRManager>();
        if (m_dlssnrManager->Init(
            m_d3d12Interop->GetDevice(),
            m_d3d12Interop->GetCommandQueue(),
            width, height))
        {
            nrInitialized = true;
            DLSS_Log("[Renderer] DLSS 5 Neural Rendering is ACTIVE (Display: %dx%d, Model Work: %dx%d, Scale: %d%%).",
                width, height, m_dlssnrManager->GetWorkWidth(), m_dlssnrManager->GetWorkHeight(), initialCfg.resolutionScale);
        }
        else
        {
            m_dlssnrManager->Cleanup();
            m_dlssnrManager.reset();
            m_d3d12Interop->Cleanup();
            m_d3d12Interop.reset();
        }
    }

    // 2. Initialize MotionVectorManager for Optical Flow in DLSS-NR AND fallback DLSS Feature 1
    m_motionVectorManager = std::make_unique<MotionVectorManager>();
    int mvW = (nrInitialized && m_dlssnrManager) ? m_dlssnrManager->GetWorkWidth() : initWorkW;
    int mvH = (nrInitialized && m_dlssnrManager) ? m_dlssnrManager->GetWorkHeight() : initWorkH;
    if (m_motionVectorManager->Init(device, mvW, mvH))
    {
        DLSS_Log("[Renderer] MotionVectorManager initialized (%dx%d) for Optical Flow.", mvW, mvH);
    }
    else
    {
        DLSS_Log("[Renderer] ERROR: Failed to initialize MotionVectorManager (%dx%d).", mvW, mvH);
    }

    if (!nrInitialized)
    {
        DLSS_Log("[Renderer] DLSS-NR unavailable; falling back to D3D11 Feature 1 DLAA & Motion Vectors.");
        m_dlssManager = std::make_unique<DLSSManager>();
        m_dlssManager->Init(device, ctx.Get(), width, height);
    }

    return true;
}


// -----------------------------------------------------------------------
// CreateSwapChain
// -----------------------------------------------------------------------
bool Renderer::CreateSwapChain(ID3D11Device* device, HWND hwnd, int width, int height)
{
    ComPtr<IDXGIDevice1> dxgiDevice;
    device->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
    dxgiDevice->SetMaximumFrameLatency(1); // reduce buffering latency

    ComPtr<IDXGIAdapter> adapter;
    dxgiDevice->GetAdapter(&adapter);

    ComPtr<IDXGIFactory2> factory2;
    adapter->GetParent(IID_PPV_ARGS(&factory2));

    // --- Check tearing (variable refresh rate) support ---
    m_tearingSupported = false;
    {
        ComPtr<IDXGIFactory5> factory5;
        if (SUCCEEDED(factory2.As(&factory5)))
        {
            BOOL tearing = FALSE;
            if (SUCCEEDED(factory5->CheckFeatureSupport(
                    DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing))))
            {
                m_tearingSupported = (tearing == TRUE);
            }
        }
    }

    // Ultra-low latency: ensure DXGI device queues at most 1 frame ahead
    ComPtr<IDXGIDevice1> dxgiDevice1;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice1))))
    {
        dxgiDevice1->SetMaximumFrameLatency(1);
    }

    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width       = static_cast<UINT>(width);
    desc.Height      = static_cast<UINT>(height);
    desc.Format      = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc  = { 1, 0 };
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling     = DXGI_SCALING_STRETCH;
    desc.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD; // lowest latency
    desc.AlphaMode   = DXGI_ALPHA_MODE_IGNORE;
    desc.Flags       = m_tearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

    HRESULT hr = factory2->CreateSwapChainForHwnd(
        device, hwnd, &desc, nullptr, nullptr, &m_swapChain);

    if (FAILED(hr))
    {
        // Fallback: blt-model swap chain (older drivers)
        desc.SwapEffect  = DXGI_SWAP_EFFECT_DISCARD;
        desc.BufferCount = 1;
        desc.Flags       = 0;
        hr = factory2->CreateSwapChainForHwnd(
            device, hwnd, &desc, nullptr, nullptr, &m_swapChain);
    }

    // Prevent DXGI from intercepting Alt+Enter
    factory2->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

    if (SUCCEEDED(hr))
    {
        // Log monitor name, vsync, and tearing support
        wchar_t monitorName[128] = L"bilinmiyor";
        {
            ComPtr<IDXGIOutput> output;
            if (SUCCEEDED(m_swapChain->GetContainingOutput(&output)))
            {
                DXGI_OUTPUT_DESC od = {};
                if (SUCCEEDED(output->GetDesc(&od)))
                    wcsncpy_s(monitorName, od.DeviceName, _TRUNCATE);
            }
        }
        DLSS_Log("[Renderer] SwapChain olusturuldu: monitor='%ls' | vsync=%s | tearing=%s | %dx%d",
            monitorName,
            m_vsyncEnabled     ? "ACIK"           : "KAPALI",
            m_tearingSupported ? "DESTEKLENIYOR"  : "DESTEKLENMIYOR",
            width, height);
    }

    return SUCCEEDED(hr);
}

// -----------------------------------------------------------------------
// CreateRTV
// -----------------------------------------------------------------------
bool Renderer::CreateRTV(ID3D11Device* device)
{
    m_rtv.Reset();
    ComPtr<ID3D11Texture2D> backBuffer;
    HRESULT hr = m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (FAILED(hr)) return false;
    hr = device->CreateRenderTargetView(backBuffer.Get(), nullptr, &m_rtv);
    return SUCCEEDED(hr);
}

// -----------------------------------------------------------------------
// CompileShaders
// -----------------------------------------------------------------------
bool Renderer::CompileShaders(ID3D11Device* device)
{
    ComPtr<ID3DBlob> blob, errBlob;

    // Vertex shader
    HRESULT hr = D3DCompile(
        s_vsSource, strlen(s_vsSource),
        "FullScreenVS", nullptr, nullptr,
        "VS", "vs_5_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
        &blob, &errBlob);

    if (FAILED(hr))
    {
        if (errBlob)
            OutputDebugStringA(static_cast<char*>(errBlob->GetBufferPointer()));
        return false;
    }
    device->CreateVertexShader(
        blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &m_vs);

    blob.Reset(); errBlob.Reset();

    // Pixel shader
    hr = D3DCompile(
        s_psSource, strlen(s_psSource),
        "FullScreenPS", nullptr, nullptr,
        "PS", "ps_5_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
        &blob, &errBlob);

    if (FAILED(hr))
    {
        if (errBlob)
            OutputDebugStringA(static_cast<char*>(errBlob->GetBufferPointer()));
        return false;
    }
    device->CreatePixelShader(
        blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &m_ps);

    return true;
}

// -----------------------------------------------------------------------
// CreateCaptureTexture
// -----------------------------------------------------------------------
bool Renderer::CreateCaptureTexture(ID3D11Device* device, int width, int height)
{
    m_captureTexture.Reset();
    m_captureSRV.Reset();

    D3D11_TEXTURE2D_DESC td = {};
    td.Width          = static_cast<UINT>(width);
    td.Height         = static_cast<UINT>(height);
    td.MipLevels      = 1;
    td.ArraySize      = 1;
    td.Format         = DXGI_FORMAT_B8G8R8A8_UNORM; // WGC always outputs BGRA8
    td.SampleDesc     = { 1, 0 };
    td.Usage          = D3D11_USAGE_DEFAULT;
    td.BindFlags      = D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = device->CreateTexture2D(&td, nullptr, &m_captureTexture);
    if (FAILED(hr)) return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvd = {};
    srvd.Format                    = DXGI_FORMAT_B8G8R8A8_UNORM;
    srvd.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvd.Texture2D.MipLevels       = 1;
    srvd.Texture2D.MostDetailedMip = 0;

    hr = device->CreateShaderResourceView(m_captureTexture.Get(), &srvd, &m_captureSRV);
    return SUCCEEDED(hr);
}

// -----------------------------------------------------------------------
// CreateFpsResources
// -----------------------------------------------------------------------
bool Renderer::CreateFpsResources(ID3D11Device* device)
{
    // 1. Create FPS texture (140x36 BGRA8)
    D3D11_TEXTURE2D_DESC td = {};
    td.Width          = kFpsWidth;
    td.Height         = kFpsHeight;
    td.MipLevels      = 1;
    td.ArraySize      = 1;
    td.Format         = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc     = { 1, 0 };
    td.Usage          = D3D11_USAGE_DEFAULT;
    td.BindFlags      = D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = device->CreateTexture2D(&td, nullptr, &m_fpsTexture);
    if (FAILED(hr)) return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvd = {};
    srvd.Format                    = DXGI_FORMAT_B8G8R8A8_UNORM;
    srvd.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvd.Texture2D.MipLevels       = 1;
    srvd.Texture2D.MostDetailedMip = 0;

    hr = device->CreateShaderResourceView(m_fpsTexture.Get(), &srvd, &m_fpsSRV);
    if (FAILED(hr)) return false;

    // 2. Create Constant Buffer
    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth      = sizeof(FpsCBufferData);
    cbd.Usage          = D3D11_USAGE_DEFAULT;
    cbd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;

    hr = device->CreateBuffer(&cbd, nullptr, &m_fpsCBuffer);
    if (FAILED(hr)) return false;

    // 3. Create GDI memory DC and DIB section
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = kFpsWidth;
    bmi.bmiHeader.biHeight      = -kFpsHeight; // top-down
    bmi.bmiHeader.biPlanes       = 1;
    bmi.bmiHeader.biBitCount     = 32;
    bmi.bmiHeader.biCompression  = BI_RGB;

    m_hFpsDC  = CreateCompatibleDC(nullptr);
    m_hFpsBmp = CreateDIBSection(m_hFpsDC, &bmi, DIB_RGB_COLORS, &m_pFpsBits, nullptr, 0);
    SelectObject(m_hFpsDC, m_hFpsBmp);

    m_lastRenderedFps  = -1;
    m_dlssnrActive     = false;
    m_isSubNative      = false;
    m_intensity        = 1.0f;
    m_colourStrength   = 1.0f;
    const auto& initialCfg = ConfigManager::Get().Config();
    m_splitEnabled     = initialCfg.splitScreen;
    m_splitPos         = initialCfg.splitPos;
    m_workTexelSize[0] = (m_width > 0) ? (1.0f / static_cast<float>(m_width)) : 0.0f;
    m_workTexelSize[1] = (m_height > 0) ? (1.0f / static_cast<float>(m_height)) : 0.0f;

    ComPtr<ID3D11DeviceContext> ctx;
    device->GetImmediateContext(&ctx);
    UpdateFpsConstantBuffer(ctx.Get());

    return true;
}

// -----------------------------------------------------------------------
// UpdateFps
// -----------------------------------------------------------------------
void Renderer::UpdateFps(ID3D11DeviceContext* ctx, int fps, bool forceRedraw)
{
    if (!m_pFpsBits || !m_hFpsDC || !m_fpsTexture) return;

    bool dlssnrOn = (m_dlssnrManager && m_dlssnrManager->IsEnabled() && m_dlssnrManager->IsEvaluating());
    bool dlssOn   = dlssnrOn || (m_dlssManager && m_dlssManager->IsEnabled() && m_dlssManager->IsAvailable());
    if (!forceRedraw && fps == m_lastRenderedFps && dlssOn == m_lastRenderedDlss && dlssnrOn == m_lastRenderedDlssNr) return;

    m_lastRenderedFps    = fps;
    m_lastRenderedDlss   = dlssOn;
    m_lastRenderedDlssNr = dlssnrOn;

    // 1. Draw rounded badge background
    HBRUSH hBgBrush   = CreateSolidBrush(RGB(18, 18, 22));
    COLORREF borderColor = dlssnrOn ? RGB(0, 230, 115) : (dlssOn ? RGB(0, 180, 90) : RGB(70, 70, 80));
    HPEN   hBorderPen = CreatePen(PS_SOLID, 1, borderColor);
    HGDIOBJ oldBrush  = SelectObject(m_hFpsDC, hBgBrush);
    HGDIOBJ oldPen    = SelectObject(m_hFpsDC, hBorderPen);
    RoundRect(m_hFpsDC, 0, 0, kFpsWidth, kFpsHeight, 10, 10);
    SelectObject(m_hFpsDC, oldPen);
    SelectObject(m_hFpsDC, oldBrush);
    DeleteObject(hBorderPen);
    DeleteObject(hBgBrush);

    // 2. Draw crisp anti-aliased font
    HFONT hFont = CreateFontW(-18, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HGDIOBJ oldFont = SelectObject(m_hFpsDC, hFont);
    SetBkMode(m_hFpsDC, TRANSPARENT);
    COLORREF textColor = dlssnrOn ? RGB(50, 255, 130) : (dlssOn ? RGB(40, 255, 110) : RGB(200, 205, 215));
    SetTextColor(m_hFpsDC, textColor);

    const wchar_t* dlssLabel = L"• OFF";
    if (dlssnrOn)
    {
        dlssLabel = L"• VLSS5";
    }
    else if (dlssOn)
    {
        dlssLabel = L"• VLSS5";
    }

    wchar_t text[48];
    if (fps > 0)
        swprintf_s(text, L"%d FPS  %s", fps, dlssLabel);
    else
        swprintf_s(text, L"-- FPS  %s", dlssLabel);

    RECT rc = { 0, 0, kFpsWidth, kFpsHeight };
    DrawTextW(m_hFpsDC, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(m_hFpsDC, oldFont);
    DeleteObject(hFont);

    // 3. Process alpha channel
    uint32_t* p = static_cast<uint32_t*>(m_pFpsBits);
    for (int i = 0; i < kFpsWidth * kFpsHeight; ++i)
    {
        uint32_t pixel = p[i];
        uint8_t r = (pixel >> 16) & 0xFF;
        uint8_t g = (pixel >> 8) & 0xFF;
        uint8_t b = pixel & 0xFF;

        if (r == 0 && g == 0 && b == 0)
        {
            p[i] = 0; // transparent outside badge
        }
        else if (g > 60 || r > 40 || b > 40)
        {
            // Text pixels: fully opaque
            p[i] = (255 << 24) | (r << 16) | (g << 8) | b;
        }
        else
        {
            // Background badge: 80% opacity
            p[i] = (200 << 24) | (r << 16) | (g << 8) | b;
        }
    }

    // 4. Upload texture to GPU
    ctx->UpdateSubresource(m_fpsTexture.Get(), 0, nullptr, m_pFpsBits, kFpsWidth * sizeof(uint32_t), 0);
    m_lastRenderedFps = fps;

    UpdateFpsConstantBuffer(ctx);
}

// -----------------------------------------------------------------------
// UpdateFpsConstantBuffer
// -----------------------------------------------------------------------
void Renderer::UpdateFpsConstantBuffer(ID3D11DeviceContext* ctx)
{
    if (!m_fpsCBuffer) return;

    FpsCBufferData cb = {};
    cb.screenSize[0]     = static_cast<float>(m_width);
    cb.screenSize[1]     = static_cast<float>(m_height);
    cb.fpsBoxSize[0]     = static_cast<float>(kFpsWidth);
    cb.fpsBoxSize[1]     = static_cast<float>(kFpsHeight);
    cb.fpsEnabled        = m_fpsEnabled ? 1 : 0;
    cb.boostFactor       = m_boostFactor;
    cb.dlssnrActive      = m_dlssnrActive ? 1 : 0;
    cb.intensity         = m_intensity;
    cb.colourStrength    = m_colourStrength;
    cb.isSubNative       = m_isSubNative ? 1 : 0;
    cb.workTexelSize[0]  = m_workTexelSize[0];
    cb.workTexelSize[1]  = m_workTexelSize[1];
    cb.splitEnabled      = m_splitEnabled ? 1 : 0;
    cb.splitPos          = m_splitPos;

    ctx->UpdateSubresource(m_fpsCBuffer.Get(), 0, nullptr, &cb, 0, 0);
    m_cbufferDirty       = false;
}

// -----------------------------------------------------------------------
// Resize — call after WGC reports a window size change
// -----------------------------------------------------------------------
void Renderer::Resize(ID3D11Device* device, int width, int height)
{
    m_width  = width;
    m_height = height;

    // Critical for D3D11: unbind RTV from immediate context before ResizeBuffers
    ComPtr<ID3D11DeviceContext> ctx;
    device->GetImmediateContext(&ctx);
    ctx->OMSetRenderTargets(0, nullptr, nullptr);

    // Detach the RTV before resize
    m_rtv.Reset();

    UINT resizeFlags = m_tearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    m_swapChain->ResizeBuffers(0,
        static_cast<UINT>(width), static_cast<UINT>(height),
        DXGI_FORMAT_UNKNOWN, resizeFlags);

    CreateRTV(device);
    UpdateFpsConstantBuffer(ctx.Get());

    if (m_dlssnrManager)
        m_dlssnrManager->Resize(width, height);
    int workW = m_dlssnrManager ? m_dlssnrManager->GetWorkWidth() : width;
    int workH = m_dlssnrManager ? m_dlssnrManager->GetWorkHeight() : height;
    if (m_d3d12Interop)
    {
        m_d3d12Interop->Resize(width, height, workW, workH);
    }
    if (m_motionVectorManager)
        m_motionVectorManager->Resize(device, workW, workH);
    if (m_dlssManager)
        m_dlssManager->Resize(device, ctx.Get(), width, height);
}

// -----------------------------------------------------------------------
// RenderFrame — blit the WGC texture to the back buffer via DLSS
// -----------------------------------------------------------------------
void Renderer::RenderFrame(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* srv)
{
    if (!m_rtv || !srv) return;

    ID3D11ShaderResourceView* renderSRV = srv;

    // 1. DLSS 5 Neural Rendering (Feature 18) via D3D12 Interop
    bool usedDlssNr = false;
    if (m_dlssnrManager && m_dlssnrManager->IsEnabled() && m_d3d12Interop)
    {
        int targetWorkW = m_dlssnrManager->GetWorkWidth();
        int targetWorkH = m_dlssnrManager->GetWorkHeight();
        if (targetWorkW > 0 && targetWorkH > 0 &&
            (m_d3d12Interop->GetWorkWidth() != targetWorkW || m_d3d12Interop->GetWorkHeight() != targetWorkH))
        {
            m_d3d12Interop->ResizeWork(targetWorkW, targetWorkH);
        }

        // Ensure MotionVectorManager matches model work resolution
        if (m_motionVectorManager && targetWorkW > 0 && targetWorkH > 0 &&
            (m_motionVectorManager->GetWidth() != targetWorkW || m_motionVectorManager->GetHeight() != targetWorkH))
        {
            ComPtr<ID3D11Device> dev;
            ctx->GetDevice(&dev);
            m_motionVectorManager->Resize(dev.Get(), targetWorkW, targetWorkH);
        }

        ComPtr<ID3D11Resource> res;
        srv->GetResource(&res);
        ComPtr<ID3D11Texture2D> inputTex;
        if (SUCCEEDED(res.As(&inputTex)) && inputTex)
        {
            MotionVectorManager* mvMgr = (m_dlssnrManager->IsOpticalFlow() && m_motionVectorManager) ? m_motionVectorManager.get() : nullptr;
            if (m_d3d12Interop->BeginFrame(inputTex.Get(), srv, mvMgr))
            {
                ID3D12Resource* mvD12 = mvMgr ? m_d3d12Interop->GetMotionD12() : nullptr;
                if (m_dlssnrManager->Evaluate(
                    m_d3d12Interop->GetCommandList(),
                    m_d3d12Interop->GetInputD12(),
                    m_d3d12Interop->GetOutputD12(),
                    mvD12))
                {
                    if (m_d3d12Interop->EndFrame())
                    {
                        renderSRV = m_d3d12Interop->GetOutputSRV();
                        usedDlssNr = true;
                    }
                }
                else
                {
                    m_d3d12Interop->GetCommandList()->Close();
                }
            }
        }
    }

    // 2. Fallback to MotionVectorManager & Feature 1 DLSS if DLSS-NR was not used
    if (!usedDlssNr && m_motionVectorManager)
    {
        m_motionVectorManager->ProcessFrame(ctx, srv);

        // Evaluate DLSS Feature 1
        if (m_dlssManager && m_dlssManager->IsEnabled() && m_dlssManager->IsAvailable())
        {
            ComPtr<ID3D11Resource> res;
            srv->GetResource(&res);
            ComPtr<ID3D11Texture2D> inputTex;
            if (SUCCEEDED(res.As(&inputTex)) && inputTex)
            {
                ID3D11ShaderResourceView* dlssOutput = m_dlssManager->Evaluate(
                    ctx,
                    srv,
                    inputTex.Get(),
                    m_motionVectorManager->GetMotionVectorsTexture(),
                    m_motionVectorManager->GetDepthTexture(),
                    m_motionVectorManager->GetUiMaskTexture());

                if (dlssOutput)
                    renderSRV = dlssOutput;
            }
        }
    }

    static uint64_t s_renderFrameCount = 0;
    s_renderFrameCount++;
    if (s_renderFrameCount <= 5 || (s_renderFrameCount % 300 == 0))
    {
        DLSS_Log("[Renderer] RenderFrame #%llu: DLSS-NR active=%d, pipeline=%s, mvMgr=%p, isOptFlow=%d, motionD12=%p",
            s_renderFrameCount,
            usedDlssNr ? 1 : 0,
            usedDlssNr ? "DLSS5_NEURAL_RENDERING" : ((renderSRV != srv) ? "DLSS_FEATURE1" : "RAW_INPUT"),
            m_motionVectorManager ? m_motionVectorManager.get() : nullptr,
            m_dlssnrManager ? (m_dlssnrManager->IsOpticalFlow() ? 1 : 0) : 0,
            (m_d3d12Interop) ? m_d3d12Interop->GetMotionD12() : nullptr);

        if (usedDlssNr && m_d3d12Interop)
        {
            m_d3d12Interop->LogDiagnosticPixels(s_renderFrameCount);
        }
    }

    // 3. Bind back buffer as render target
    ctx->OMSetRenderTargets(1, m_rtv.GetAddressOf(), nullptr);

    D3D11_VIEWPORT vp = {};
    vp.Width    = static_cast<float>(m_width);
    vp.Height   = static_cast<float>(m_height);
    vp.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &vp);

    // Set shaders
    ctx->VSSetShader(m_vs.Get(), nullptr, 0);
    ctx->PSSetShader(m_ps.Get(), nullptr, 0);

    bool dlssnrActive = usedDlssNr;
    bool isSubNative = (usedDlssNr && m_dlssnrManager && m_dlssnrManager->GetResolutionScale() < 0.999f);
    float intensity = (m_dlssnrManager && usedDlssNr) ? m_dlssnrManager->GetIntensity() : 1.0f;
    float colourStrength = 1.0f;
    int workW = (m_dlssnrManager && usedDlssNr) ? m_dlssnrManager->GetWorkWidth() : m_width;
    int workH = (m_dlssnrManager && usedDlssNr) ? m_dlssnrManager->GetWorkHeight() : m_height;
    if (workW <= 0) workW = m_width;
    if (workH <= 0) workH = m_height;

    float texelX = (workW > 0) ? (1.0f / static_cast<float>(workW)) : 0.0f;
    float texelY = (workH > 0) ? (1.0f / static_cast<float>(workH)) : 0.0f;

    if (m_dlssnrActive != dlssnrActive || m_isSubNative != isSubNative ||
        m_intensity != intensity || m_colourStrength != colourStrength ||
        m_workTexelSize[0] != texelX || m_workTexelSize[1] != texelY)
    {
        m_dlssnrActive     = dlssnrActive;
        m_isSubNative      = isSubNative;
        m_intensity        = intensity;
        m_colourStrength   = colourStrength;
        m_workTexelSize[0] = texelX;
        m_workTexelSize[1] = texelY;
        m_cbufferDirty     = true;
    }

    if (m_cbufferDirty)
    {
        UpdateFpsConstantBuffer(ctx);
    }

    // Bind textures for Matched Residual Resolve:
    // t0: Model Output (work resolution)
    // t1: Model Input Proxy (work resolution)
    // t2: Original Native Game Frame (full display resolution)
    // t3: FPS Counter Texture
    ID3D11ShaderResourceView* srvs[4];
    if (usedDlssNr && m_d3d12Interop)
    {
        srvs[0] = m_d3d12Interop->GetOutputSRV();
        srvs[1] = m_d3d12Interop->GetRawInputSRV();
        srvs[2] = srv;
        srvs[3] = m_fpsSRV.Get();
    }
    else
    {
        srvs[0] = renderSRV;
        srvs[1] = renderSRV;
        srvs[2] = renderSRV;
        srvs[3] = m_fpsSRV.Get();
    }
    ctx->PSSetShaderResources(0, 4, srvs);

    // Bind samplers: s0 = Point sampler (for native original), s1 = Linear sampler (for model/proxy)
    ID3D11SamplerState* samplers[2] = { m_sampler.Get(), m_linearSampler.Get() };
    ctx->PSSetSamplers(0, 2, samplers);
    ctx->PSSetConstantBuffers(0, 1, m_fpsCBuffer.GetAddressOf());

    // Full-screen triangle — renders Matched Residual DLSS 5 frame with RCAS and draws FPS badge ON TOP
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->Draw(3, 0);

    // Clear SRVs to prevent pipeline hazards
    ID3D11ShaderResourceView* nullSRVs[4] = { nullptr, nullptr, nullptr, nullptr };
    ctx->PSSetShaderResources(0, 4, nullSRVs);
}

// -----------------------------------------------------------------------
// Present
// -----------------------------------------------------------------------
void Renderer::Present()
{
    LARGE_INTEGER t0, t1, freq;
    QueryPerformanceCounter(&t0);

    UINT syncInterval = m_vsyncEnabled ? 1u : 0u;
    UINT presentFlags = (!m_vsyncEnabled && m_tearingSupported) ? DXGI_PRESENT_ALLOW_TEARING : 0u;
    HRESULT hr = m_swapChain->Present(syncInterval, presentFlags);

    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&freq);
    double presentMs = double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(freq.QuadPart);

    if (presentMs > 50.0)
        DLSS_Log("[Present] UYARI: Present() %.2f ms surdu (vsync=%s, tearing=%s)",
            presentMs,
            m_vsyncEnabled     ? "ACIK" : "KAPALI",
            m_tearingSupported ? "DESTEKLENIYOR" : "DESTEKLENMIYOR");

    if (hr == DXGI_STATUS_OCCLUDED)
        DLSS_Log("[Present] UYARI: DXGI_STATUS_OCCLUDED - overlay penceresi baska pencere tarafindan kapatildi");
}

// -----------------------------------------------------------------------
// Cleanup
// -----------------------------------------------------------------------
void Renderer::Cleanup()
{
    if (m_dlssnrManager)
    {
        m_dlssnrManager->Cleanup();
        m_dlssnrManager.reset();
    }

    if (m_d3d12Interop)
    {
        m_d3d12Interop->Cleanup();
        m_d3d12Interop.reset();
    }

    if (m_dlssManager)
    {
        m_dlssManager->Cleanup();
        m_dlssManager.reset();
    }

    if (m_motionVectorManager)
    {
        m_motionVectorManager->Cleanup();
        m_motionVectorManager.reset();
    }

    if (m_hFpsBmp) { DeleteObject(m_hFpsBmp); m_hFpsBmp = nullptr; }
    if (m_hFpsDC)  { DeleteDC(m_hFpsDC);       m_hFpsDC  = nullptr; }
    m_pFpsBits        = nullptr;
    m_lastRenderedFps = -1;

    m_fpsCBuffer.Reset();
    m_fpsSRV.Reset();
    m_fpsTexture.Reset();
    m_captureSRV.Reset();
    m_captureTexture.Reset();
    m_linearSampler.Reset();
    m_sampler.Reset();
    m_ps.Reset();
    m_vs.Reset();
    m_rtv.Reset();
    m_swapChain.Reset();
}

