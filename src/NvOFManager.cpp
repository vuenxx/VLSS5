#include "NvOFManager.h"
#include <cstdio>
#include <algorithm>

// ---------------------------------------------------------------------------
// Format converter compute shader:
//   Converts NVOF hardware S16.5 fixed-point (1.0 px = 32.0 units) to
//   floating-point pixel motion vectors in targetMvUAV (R16G16_FLOAT).
// ---------------------------------------------------------------------------
static const char* s_convertCS = R"HLSL(
Texture2D<int2>     g_RawMV  : register(t0);
RWTexture2D<float2> g_OutMV  : register(u0);

[numthreads(16, 16, 1)]
void CSConvert(uint3 dtid : SV_DispatchThreadID)
{
    uint width, height;
    g_OutMV.GetDimensions(width, height);
    if (dtid.x >= width || dtid.y >= height)
        return;

    // NVOF GridSize=4 outputs one vector per 4x4 pixel block.
    // Shift right by 2 (divide by 4) to get the corresponding 4x4 grid coordinate.
    int2 raw = g_RawMV[dtid.xy >> 2];

    // NVOF outputs vectors in S16.5 fixed-point format (1 pixel = 32 raw integer units)
    // Convert to pixel float coordinates (divide by 32.0f)
    g_OutMV[dtid.xy] = float2(raw) * 0.03125f;
}
)HLSL";

// ---------------------------------------------------------------------------
// LoadNVOFEntryPoints
// ---------------------------------------------------------------------------
bool NvOFManager::LoadNVOFEntryPoints()
{
    if (m_hNvOfDll && m_nvof.nvCreateOpticalFlowD3D11) return true;

    // Search in current directory (where nvofapi64.dll is located) or system path
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(exePath, L'\\');
    if (lastSlash) *(lastSlash + 1) = L'\0';

    std::wstring localDll = std::wstring(exePath) + L"nvofapi64.dll";
    m_hNvOfDll = LoadLibraryW(localDll.c_str());
    if (!m_hNvOfDll)
    {
        m_hNvOfDll = LoadLibraryW(L"nvofapi64.dll");
    }

    if (!m_hNvOfDll)
    {
        DLSS_Log("[NVOF] nvofapi64.dll could not be loaded. Hardware Optical Flow unavailable.");
        return false;
    }

    auto pfnCreateD3D11 = (NV_OF_STATUS(NVOFAPI*)(uint32_t, NV_OF_D3D11_API_FUNCTION_LIST*))
        GetProcAddress(m_hNvOfDll, "NvOFAPICreateInstanceD3D11");

    if (!pfnCreateD3D11)
    {
        DLSS_Log("[NVOF] NvOFAPICreateInstanceD3D11 entry point not found in nvofapi64.dll.");
        return false;
    }

    NV_OF_STATUS st = pfnCreateD3D11(NV_OF_API_VERSION, &m_nvof);
    if (st != NV_OF_SUCCESS || !m_nvof.nvCreateOpticalFlowD3D11 || !m_nvof.nvOFInit || !m_nvof.nvOFExecute)
    {
        DLSS_Log("[NVOF] Failed to initialize NVOF API function list (status 0x%08X).", st);
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// CompileConvertShader
// ---------------------------------------------------------------------------
bool NvOFManager::CompileConvertShader(ID3D11Device* device)
{
    if (m_convertCS) return true;

    ComPtr<ID3DBlob> blob, errBlob;
    HRESULT hr = D3DCompile(
        s_convertCS, strlen(s_convertCS),
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

    hr = device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &m_convertCS);
    return SUCCEEDED(hr);
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
    // For GridSize=4, NVOF produces an output grid of size ceil(width/4) x ceil(height/4)
    uint32_t outWidth  = (static_cast<uint32_t>(width)  + 3) / 4;
    uint32_t outHeight = (static_cast<uint32_t>(height) + 3) / 4;

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

    // 3. Register GPU resources with NVOF hardware engine (performed ONCE)
    NV_OF_STATUS st = m_nvof.nvOFRegisterResourceD3D11(m_hOf, m_curInputTex.Get(), &m_hCurInputBuffer);
    if (st != NV_OF_SUCCESS || !m_hCurInputBuffer)
    {
        DLSS_Log("[NVOF] nvOFRegisterResourceD3D11(CurrentFrame) failed: 0x%08X", st);
        return false;
    }

    st = m_nvof.nvOFRegisterResourceD3D11(m_hOf, m_prevInputTex.Get(), &m_hPrevInputBuffer);
    if (st != NV_OF_SUCCESS || !m_hPrevInputBuffer)
    {
        DLSS_Log("[NVOF] nvOFRegisterResourceD3D11(PrevFrame) failed: 0x%08X", st);
        return false;
    }

    st = m_nvof.nvOFRegisterResourceD3D11(m_hOf, m_rawMvTex.Get(), &m_hRawMvBuffer);
    if (st != NV_OF_SUCCESS || !m_hRawMvBuffer)
    {
        DLSS_Log("[NVOF] nvOFRegisterResourceD3D11(OutputRawMV) failed: 0x%08X", st);
        return false;
    }

    // 4. Initialize hardware optical flow session with GridSize=4 (OFA ASIC fast path, ~0.5ms)
    NV_OF_INIT_PARAMS initParams = { 0 };
    initParams.width             = static_cast<uint32_t>(width);
    initParams.height            = static_cast<uint32_t>(height);
    initParams.mode              = NV_OF_MODE_OPTICALFLOW;
    initParams.perfLevel         = NV_OF_PERF_LEVEL_FAST;
    initParams.outGridSize       = NV_OF_OUTPUT_VECTOR_GRID_SIZE_4; // 4x4 grid (6.5x faster than 1x1 on Ada/Ampere)
    initParams.inputBufferFormat = NV_OF_BUFFER_FORMAT_ABGR8;       // Native DXGI_FORMAT_B8G8R8A8_UNORM

    st = m_nvof.nvOFInit(m_hOf, &initParams);
    if (st != NV_OF_SUCCESS)
    {
        char errBuf[256] = { 0 };
        uint32_t errLen = sizeof(errBuf);
        m_nvof.nvOFGetLastError(m_hOf, errBuf, &errLen);
        DLSS_Log("[NVOF] nvOFInit failed: 0x%08X (%s)", st, errBuf);
        return false;
    }

    DLSS_Log("[NVOF] Hardware session initialized (%ux%u, GridSize=4 [%ux%u], Format=B8G8R8A8_UNORM, Fast OFA path)!",
        width, height, outWidth, outHeight);
    return true;
}

// ---------------------------------------------------------------------------
// UnregisterResources
// ---------------------------------------------------------------------------
void NvOFManager::UnregisterResources()
{
    if (m_nvof.nvOFUnregisterResourceD3D11)
    {
        if (m_hCurInputBuffer)  { m_nvof.nvOFUnregisterResourceD3D11(m_hCurInputBuffer);  m_hCurInputBuffer = nullptr; }
        if (m_hPrevInputBuffer) { m_nvof.nvOFUnregisterResourceD3D11(m_hPrevInputBuffer); m_hPrevInputBuffer = nullptr; }
        if (m_hRawMvBuffer)     { m_nvof.nvOFUnregisterResourceD3D11(m_hRawMvBuffer);     m_hRawMvBuffer = nullptr; }
    }

    m_rawMvSRV.Reset();
    m_rawMvTex.Reset();
    m_prevInputTex.Reset();
    m_curInputTex.Reset();
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
bool NvOFManager::Init(ID3D11Device* device, ID3D11DeviceContext* context, int width, int height)
{
    Cleanup();

    if (!device || !context || width <= 0 || height <= 0) return false;

    if (!LoadNVOFEntryPoints()) return false;
    if (!CompileConvertShader(device)) return false;

    NV_OF_STATUS st = m_nvof.nvCreateOpticalFlowD3D11(device, context, &m_hOf);
    if (st != NV_OF_SUCCESS || !m_hOf)
    {
        DLSS_Log("[NVOF] nvCreateOpticalFlowD3D11 failed (status 0x%08X). Hardware OFA not supported on this adapter.", st);
        return false;
    }

    if (!CreateAndRegisterResources(device, width, height))
    {
        Cleanup();
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

    if (!m_initialized || !m_hOf)
    {
        Init(device, context, width, height);
        return;
    }

    CreateAndRegisterResources(device, width, height);
}

// ---------------------------------------------------------------------------
// Cleanup
// ---------------------------------------------------------------------------
void NvOFManager::Cleanup()
{
    UnregisterResources();

    if (m_hOf && m_nvof.nvOFDestroy)
    {
        m_nvof.nvOFDestroy(m_hOf);
        m_hOf = nullptr;
    }

    m_convertCS.Reset();

    if (m_hNvOfDll)
    {
        FreeLibrary(m_hNvOfDll);
        m_hNvOfDll = nullptr;
    }

    memset(&m_nvof, 0, sizeof(m_nvof));
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
    if (!m_initialized || !m_hOf || !context || !currentFrameTex || !targetMvUAV)
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

    NV_OF_STATUS st = m_nvof.nvOFExecute(m_hOf, &inParams, &outParams);
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
    if (m_convertCS && m_rawMvSRV)
    {
        context->CSSetShader(m_convertCS.Get(), nullptr, 0);

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
