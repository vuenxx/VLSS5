#include "NvOFManager.h"
#include <cstdio>
#include <algorithm>

// ---------------------------------------------------------------------------
// Format converter compute shader:
//   Converts NVOF hardware S16.5 fixed-point (1.0 px = 32.0 units) to
//   floating-point pixel motion vectors in targetMvUAV (R16G16_FLOAT).
// ---------------------------------------------------------------------------
static const char* kConvertCSSource = R"HLSL(
Texture2D<int2>     g_RawMV  : register(t0);
RWTexture2D<float2> g_OutMV  : register(u0);

cbuffer ConvertParams : register(b0)
{
    uint  g_gridSize;
    uint3 g_pad;
};

[numthreads(16, 16, 1)]
void CSConvert(uint3 dtid : SV_DispatchThreadID)
{
    uint width, height;
    g_OutMV.GetDimensions(width, height);
    if (dtid.x >= width || dtid.y >= height)
        return;

    // NVOF outputs one vector per g_gridSize x g_gridSize pixel block.
    // gridSize artik yapilandirilabilir (1/2/4), o yuzden sabit >>2 yerine bolme.
    int2 raw = g_RawMV[dtid.xy / g_gridSize];

    // NVOF outputs vectors in S16.5 fixed-point format (1 pixel = 32 raw integer units)
    // Convert to pixel float coordinates (divide by 32.0f)
    g_OutMV[dtid.xy] = float2(raw) * 0.03125f;
}
)HLSL";

// ---------------------------------------------------------------------------
// LoadNVOFEntryPoints
// ---------------------------------------------------------------------------
// ---- process-omurlu paylasilan durum (bkz. NvOFManager.h aciklamasi) ----
HMODULE                       NvOFManager::s_hNvOfDll   = nullptr;
NV_OF_D3D11_API_FUNCTION_LIST NvOFManager::s_nvof       = { 0 };
NvOFHandle                    NvOFManager::s_hOf        = nullptr;
ID3D11Device*                 NvOFManager::s_ofDevice   = nullptr;
ID3D11DeviceContext*          NvOFManager::s_ofContext  = nullptr;
int                           NvOFManager::s_ofWidth    = 0;
int                           NvOFManager::s_ofHeight   = 0;
int                           NvOFManager::s_ofGridSize = 0;
NV_OF_PERF_LEVEL              NvOFManager::s_ofPerfLevel = NV_OF_PERF_LEVEL_FAST;
ComPtr<ID3D11ComputeShader>   NvOFManager::s_convertCS;

bool NvOFManager::LoadNVOFEntryPoints()
{
    if (s_hNvOfDll && s_nvof.nvCreateOpticalFlowD3D11) return true;

    // Search in current directory (where nvofapi64.dll is located) or system path
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(exePath, L'\\');
    if (lastSlash) *(lastSlash + 1) = L'\0';

    std::wstring localDll = std::wstring(exePath) + L"nvofapi64.dll";
    s_hNvOfDll = LoadLibraryW(localDll.c_str());
    if (!s_hNvOfDll)
    {
        s_hNvOfDll = LoadLibraryW(L"nvofapi64.dll");
    }

    if (!s_hNvOfDll)
    {
        DLSS_Log("[NVOF] nvofapi64.dll could not be loaded. Hardware Optical Flow unavailable.");
        return false;
    }

    auto pfnCreateD3D11 = (NV_OF_STATUS(NVOFAPI*)(uint32_t, NV_OF_D3D11_API_FUNCTION_LIST*))
        GetProcAddress(s_hNvOfDll, "NvOFAPICreateInstanceD3D11");

    if (!pfnCreateD3D11)
    {
        DLSS_Log("[NVOF] NvOFAPICreateInstanceD3D11 entry point not found in nvofapi64.dll.");
        return false;
    }

    NV_OF_STATUS st = pfnCreateD3D11(NV_OF_API_VERSION, &s_nvof);
    if (st != NV_OF_SUCCESS || !s_nvof.nvCreateOpticalFlowD3D11 || !s_nvof.nvOFInit || !s_nvof.nvOFExecute)
    {
        DLSS_Log("[NVOF] Failed to initialize NVOF API function list (status 0x%08X).", st);
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// EnsureOFContext
//   Donanim Optical Flow oturumunu (nvCreateOpticalFlowD3D11 + nvOFInit) SADECE
//   hedef cihaz/boyut/kalite bir onceki basariyla kurulmus static baglamdan
//   FARKLIYSA sifirdan kurar; aksi halde (ayni sureç icinde onceki bir
//   session'dan kalma) mevcut s_hOf'u oldugu gibi yeniden kullanir. nvOFInit
//   tek basina saniyeler surebilen, GPU/DWM'i kilitleyen senkron bir surucu
//   cagrisi oldugundan bu, her "Baslat/Durdur" dongusunde tekrarini onlemek
//   icin var (bkz. NvOFManager.h'deki uzun aciklama).
// ---------------------------------------------------------------------------
bool NvOFManager::EnsureOFContext(ID3D11Device* device, ID3D11DeviceContext* context, int width, int height)
{
    if (s_hOf && s_ofDevice == device && s_ofContext == context &&
        s_ofWidth == width && s_ofHeight == height &&
        s_ofGridSize == m_gridSize && s_ofPerfLevel == m_perfLevel)
    {
        DLSS_Log("[NVOF] Mevcut donanim oturumu yeniden kullaniliyor (%dx%d, GridSize=%d) -- nvOFInit atlandi.",
            width, height, m_gridSize);
        return true;
    }

    if (s_hOf && s_nvof.nvOFDestroy)
    {
        s_nvof.nvOFDestroy(s_hOf);
        s_hOf = nullptr;
    }

    NV_OF_STATUS st = s_nvof.nvCreateOpticalFlowD3D11(device, context, &s_hOf);
    if (st != NV_OF_SUCCESS || !s_hOf)
    {
        DLSS_Log("[NVOF] nvCreateOpticalFlowD3D11 failed (status 0x%08X). Hardware OFA not supported on this adapter.", st);
        s_hOf = nullptr;
        return false;
    }

    NV_OF_INIT_PARAMS initParams = { 0 };
    initParams.width             = static_cast<uint32_t>(width);
    initParams.height            = static_cast<uint32_t>(height);
    initParams.mode              = NV_OF_MODE_OPTICALFLOW;
    initParams.perfLevel         = m_perfLevel;
    initParams.outGridSize       = static_cast<NV_OF_OUTPUT_VECTOR_GRID_SIZE>(m_gridSize);
    initParams.inputBufferFormat = NV_OF_BUFFER_FORMAT_ABGR8;

    st = s_nvof.nvOFInit(s_hOf, &initParams);
    if (st != NV_OF_SUCCESS)
    {
        char errBuf[256] = { 0 };
        uint32_t errLen = sizeof(errBuf);
        s_nvof.nvOFGetLastError(s_hOf, errBuf, &errLen);
        DLSS_Log("[NVOF] nvOFInit failed: 0x%08X (%s)", st, errBuf);
        s_nvof.nvOFDestroy(s_hOf);
        s_hOf = nullptr;
        return false;
    }

    s_ofDevice    = device;
    s_ofContext   = context;
    s_ofWidth     = width;
    s_ofHeight    = height;
    s_ofGridSize  = m_gridSize;
    s_ofPerfLevel = m_perfLevel;

    DLSS_Log("[NVOF] Hardware session initialized (%ux%u, GridSize=%d, PerfLevel=%d, Format=B8G8R8A8_UNORM)!",
        width, height, m_gridSize, (int)m_perfLevel);
    return true;
}

// ---------------------------------------------------------------------------
// CompileConvertShader
// ---------------------------------------------------------------------------
bool NvOFManager::CompileConvertShader(ID3D11Device* device)
{
    if (!s_convertCS)
    {
        ComPtr<ID3DBlob> blob, errBlob;
        HRESULT hr = D3DCompile(
            kConvertCSSource, strlen(kConvertCSSource),
            "NVOF_ConvertCS", nullptr, nullptr,
            "CSConvert", "cs_5_0",
            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
            &blob, &errBlob);

        if (FAILED(hr))
        {
            if (errBlob)
                DLSS_Log("[NVOF] ERROR compiling ConvertCS: %s", (char*)errBlob->GetBufferPointer());
            return false;
        }

        hr = device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &s_convertCS);
        if (FAILED(hr)) return false;
    }

    if (!m_convertParamsCB)
    {
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = 16; // uint g_gridSize + uint3 padding, 16-byte cbuffer aligned
        bd.Usage     = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        HRESULT hr = device->CreateBuffer(&bd, nullptr, &m_convertParamsCB);
        if (FAILED(hr)) return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// CreateAndRegisterResources
//   CRITICAL: Resources are registered ONCE here, NOT per frame!
// ---------------------------------------------------------------------------
bool NvOFManager::CreateAndRegisterResources(ID3D11Device* device, int width, int height)
{
    UnregisterResources();

    m_width  = width;
    m_height = height;

    // 1. Current & Previous input textures (DXGI_FORMAT_B8G8R8A8_UNORM matches NV_OF_BUFFER_FORMAT_ABGR8 natively)
    D3D11_TEXTURE2D_DESC tdIn = {};
    tdIn.Width          = static_cast<UINT>(width);
    tdIn.Height         = static_cast<UINT>(height);
    tdIn.MipLevels      = 1;
    tdIn.ArraySize      = 1;
    tdIn.Format         = DXGI_FORMAT_B8G8R8A8_UNORM;
    tdIn.SampleDesc     = { 1, 0 };
    tdIn.Usage          = D3D11_USAGE_DEFAULT;
    tdIn.BindFlags      = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

    HRESULT hr = device->CreateTexture2D(&tdIn, nullptr, &m_curInputTex);
    if (FAILED(hr)) return false;

    hr = device->CreateTexture2D(&tdIn, nullptr, &m_prevInputTex);
    if (FAILED(hr)) return false;

    // 2. Raw NVOF hardware output texture (DXGI_FORMAT_R16G16_SINT, S16.5 format)
    // NVOF produces an output grid of size ceil(width/gridSize) x ceil(height/gridSize)
    uint32_t outWidth  = (static_cast<uint32_t>(width)  + m_gridSize - 1) / m_gridSize;
    uint32_t outHeight = (static_cast<uint32_t>(height) + m_gridSize - 1) / m_gridSize;

    D3D11_TEXTURE2D_DESC tdOut = {};
    tdOut.Width          = outWidth;
    tdOut.Height         = outHeight;
    tdOut.MipLevels      = 1;
    tdOut.ArraySize      = 1;
    tdOut.Format         = DXGI_FORMAT_R16G16_SINT;
    tdOut.SampleDesc     = { 1, 0 };
    tdOut.Usage          = D3D11_USAGE_DEFAULT;
    tdOut.BindFlags      = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

    hr = device->CreateTexture2D(&tdOut, nullptr, &m_rawMvTex);
    if (FAILED(hr)) return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvd = {};
    srvd.Format                    = DXGI_FORMAT_R16G16_SINT;
    srvd.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvd.Texture2D.MipLevels       = 1;
    srvd.Texture2D.MostDetailedMip = 0;

    hr = device->CreateShaderResourceView(m_rawMvTex.Get(), &srvd, &m_rawMvSRV);
    if (FAILED(hr)) return false;

    // 3. Register GPU resources with NVOF hardware engine (performed ONCE per session,
    //    against the possibly-REUSED static s_hOf -- bkz. EnsureOFContext)
    NV_OF_STATUS st = s_nvof.nvOFRegisterResourceD3D11(s_hOf, m_curInputTex.Get(), &m_hCurInputBuffer);
    if (st != NV_OF_SUCCESS || !m_hCurInputBuffer)
    {
        DLSS_Log("[NVOF] nvOFRegisterResourceD3D11(CurrentFrame) failed: 0x%08X", st);
        return false;
    }

    st = s_nvof.nvOFRegisterResourceD3D11(s_hOf, m_prevInputTex.Get(), &m_hPrevInputBuffer);
    if (st != NV_OF_SUCCESS || !m_hPrevInputBuffer)
    {
        DLSS_Log("[NVOF] nvOFRegisterResourceD3D11(PrevFrame) failed: 0x%08X", st);
        return false;
    }

    st = s_nvof.nvOFRegisterResourceD3D11(s_hOf, m_rawMvTex.Get(), &m_hRawMvBuffer);
    if (st != NV_OF_SUCCESS || !m_hRawMvBuffer)
    {
        DLSS_Log("[NVOF] nvOFRegisterResourceD3D11(OutputRawMV) failed: 0x%08X", st);
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// UnregisterResources
// ---------------------------------------------------------------------------
void NvOFManager::UnregisterResources()
{
    if (s_nvof.nvOFUnregisterResourceD3D11)
    {
        if (m_hCurInputBuffer)  { s_nvof.nvOFUnregisterResourceD3D11(m_hCurInputBuffer);  m_hCurInputBuffer = nullptr; }
        if (m_hPrevInputBuffer) { s_nvof.nvOFUnregisterResourceD3D11(m_hPrevInputBuffer); m_hPrevInputBuffer = nullptr; }
        if (m_hRawMvBuffer)     { s_nvof.nvOFUnregisterResourceD3D11(m_hRawMvBuffer);     m_hRawMvBuffer = nullptr; }
    }

    m_rawMvSRV.Reset();
    m_rawMvTex.Reset();
    m_prevInputTex.Reset();
    m_curInputTex.Reset();
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
bool NvOFManager::Init(ID3D11Device* device, ID3D11DeviceContext* context, int width, int height,
    int gridSize, NV_OF_PERF_LEVEL perfLevel)
{
    // NOT: eskiden burada kosulsuz Cleanup() cagrilirdi -- bu, static (process-
    // omurlu) s_hOf'u da yikip her session'da sifirdan nvOFInit'e (4-9 sn, GPU/DWM
    // kilitleyen) zorluyordu. Artik yalnizca BU ORNEGE ait (session'a ozel)
    // kaynaklari (dokular/registrasyonlar) serbest birakiyoruz; donanim baglami
    // EnsureOFContext icinde, GERCEKTEN gerekliyse yikilip kuruluyor.
    UnregisterResources();
    m_initialized = false;

    if (!device || !context || width <= 0 || height <= 0) return false;

    // gridSize NV_OF_OUTPUT_VECTOR_GRID_SIZE degerleriyle (1/2/4) birebir eslesmek
    // zorunda; gecersiz deger sessizce 4'e duser.
    m_gridSize  = (gridSize == 1 || gridSize == 2 || gridSize == 4) ? gridSize : 4;
    m_perfLevel = perfLevel;

    if (!LoadNVOFEntryPoints()) return false;
    if (!CompileConvertShader(device)) return false;

    if (!EnsureOFContext(device, context, width, height))
    {
        return false;
    }

    if (!CreateAndRegisterResources(device, width, height))
    {
        UnregisterResources();
        return false;
    }

    m_initialized = true;
    return true;
}

// ---------------------------------------------------------------------------
// Resize
// ---------------------------------------------------------------------------
void NvOFManager::Resize(ID3D11Device* device, ID3D11DeviceContext* context, int width, int height)
{
    if (m_width == width && m_height == height && m_initialized) return;

    // ONEMLI: nvOFInit() AYNI, zaten calisan s_hOf handle'i uzerinde farkli
    // bir boyutla TEKRAR cagrilamaz -- surucu CNvOFDeviceD3D11:Initialize'da
    // E_FAIL (0x80004005) donuyor (loglarla dogrulandi: olcek slider'i %99'a
    // her cekildiginde bu hata basiyor). Bu basarisizliktan sonra HER
    // nvOFExecute cagrisi da basarisiz olmaya devam ediyor; MotionVectorManager
    // bunu sessizce yazilim (8x8 groupshared block-matching) fallback'ine
    // devrederek maskeliyor -- cok daha gurultulu/yanlis vektorler uretiyor,
    // "hareket sirasinda golgeler yer degistiriyor" sikayetinin dogrudan
    // kaynagi bu. gridSize/perfLevel'i acikca koruyarak geciyoruz; Init()
    // bunlari parametreden yazar, gecmezsek varsayilanlara (grid4/FAST)
    // sessizce duser, oradaki secim kaybolur. Boyut GERCEKTEN degisirse
    // Init() -> EnsureOFContext() bunu tespit edip s_hOf'u yikip sifirdan
    // kurar; degismezse (asil optimize edilen ortak durum: ayni oturumu
    // durdurup yeniden baslatmak) donanim baglami OLDUGU GIBI yeniden kullanilir.
    Init(device, context, width, height, m_gridSize, m_perfLevel);
}

// ---------------------------------------------------------------------------
// Cleanup
// ---------------------------------------------------------------------------
void NvOFManager::Cleanup()
{
    // NOT: static s_hOf/s_nvof/s_hNvOfDll'e KASITLI OLARAK dokunmuyoruz -- bunlar
    // process-omurlu, paylasilan donanim baglami (bkz. NvOFManager.h ve Init()
    // aciklamalari). Yalnizca BU ORNEGE ait session-seviyesi kaynaklari
    // (dokular, registre edilmis buffer'lar) serbest birakiyoruz.
    UnregisterResources();

    // s_convertCS static/process-omurlu -- kasitli olarak sifirlanmiyor.
    m_convertParamsCB.Reset();

    m_initialized = false;
}

// ---------------------------------------------------------------------------
// ProcessFrame
//   Executes hardware optical flow and converts output directly into targetMvUAV.
//   ZERO dynamic resource registration: executes purely on pre-registered buffers.
// ---------------------------------------------------------------------------
bool NvOFManager::ProcessFrame(
    ID3D11DeviceContext* context,
    ID3D11Texture2D* currentFrameTex,
    ID3D11Texture2D* prevFrameTex,
    ID3D11UnorderedAccessView* targetMvUAV,
    ID3D11Texture2D* targetMvTex)
{
    if (!m_initialized || !s_hOf || !context || !currentFrameTex || !targetMvUAV)
        return false;

    // 1. Copy captured frames into pre-registered hardware input buffers
    context->CopyResource(m_curInputTex.Get(), currentFrameTex);
    if (prevFrameTex)
    {
        context->CopyResource(m_prevInputTex.Get(), prevFrameTex);
    }
    else
    {
        context->CopyResource(m_prevInputTex.Get(), m_curInputTex.Get());
    }

    // 2. Execute hardware optical flow on dedicated OFA ASIC silicon
    NV_OF_EXECUTE_INPUT_PARAMS inParams = { 0 };
    inParams.inputFrame           = m_hCurInputBuffer;
    inParams.referenceFrame       = m_hPrevInputBuffer;
    inParams.disableTemporalHints = NV_OF_FALSE;

    NV_OF_EXECUTE_OUTPUT_PARAMS outParams = { 0 };
    outParams.outputBuffer        = m_hRawMvBuffer;

    NV_OF_STATUS st = s_nvof.nvOFExecute(s_hOf, &inParams, &outParams);
    if (st != NV_OF_SUCCESS)
    {
        static uint32_t s_failCount = 0;
        if (++s_failCount <= 5 || (s_failCount % 300 == 0))
        {
            DLSS_Log("[NVOF] WARNING: nvOFExecute failed with status 0x%08X", st);
        }
        return false;
    }

    // 3. Format Conversion: Scale S16.5 fixed-point to R16G16_FLOAT directly in targetMvUAV
    // (D3D11 -> D3D12 Shared NT Handle target texture)
    if (s_convertCS && m_rawMvSRV)
    {
        if (m_convertParamsCB)
        {
            uint32_t cb[4] = { static_cast<uint32_t>(m_gridSize), 0, 0, 0 };
            context->UpdateSubresource(m_convertParamsCB.Get(), 0, nullptr, cb, 0, 0);
            context->CSSetConstantBuffers(0, 1, m_convertParamsCB.GetAddressOf());
        }

        context->CSSetShader(s_convertCS.Get(), nullptr, 0);

        ID3D11ShaderResourceView* srvs[1] = { m_rawMvSRV.Get() };
        context->CSSetShaderResources(0, 1, srvs);

        ID3D11UnorderedAccessView* uavs[1] = { targetMvUAV };
        UINT initCounts[1] = { 0 };
        context->CSSetUnorderedAccessViews(0, 1, uavs, initCounts);

        UINT groupsX = (static_cast<UINT>(m_width)  + 15) / 16;
        UINT groupsY = (static_cast<UINT>(m_height) + 15) / 16;
        context->Dispatch(groupsX, groupsY, 1);

        // Unbind resources to prevent hazards
        ID3D11UnorderedAccessView* nullUAVs[1] = { nullptr };
        context->CSSetUnorderedAccessViews(0, 1, nullUAVs, nullptr);

        ID3D11ShaderResourceView* nullSRVs[1] = { nullptr };
        context->CSSetShaderResources(0, 1, nullSRVs);
    }

    return true;
}
