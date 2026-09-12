#include "DLSSManager.h"
#include <shlwapi.h>
#include <cstdio>
#include <cstdarg>
#include <mutex>

#pragma comment(lib, "shlwapi.lib")

static std::mutex g_logMutex;

void DLSS_Log(const char* fmt, ...)
{
    std::lock_guard<std::mutex> lock(g_logMutex);

    static FILE* s_logFile = nullptr;
    static bool s_first = true;
    if (s_first)
    {
        s_first = false;
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        PathRemoveFileSpecW(path);
        PathCombineW(path, path, L"vlss5_logs.log");
        s_logFile = _wfsopen(path, L"w", _SH_DENYNO);
        if (s_logFile)
        {
            fputs("===============================================================================\n", s_logFile);
            fputs(" VLSS5 - Native NVIDIA NGX DLSS Diagnostic Log\n", s_logFile);
            fputs("===============================================================================\n", s_logFile);
            fflush(s_logFile);
        }
    }

    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    SYSTEMTIME st;
    GetLocalTime(&st);
    char fullMsg[1200];
    snprintf(fullMsg, sizeof(fullMsg), "[%02d:%02d:%02d.%03d] %s\n",
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, buf);

    OutputDebugStringA(fullMsg);
    if (s_logFile)
    {
        fputs(fullMsg, s_logFile);
        fflush(s_logFile);
    }
}

// ---------------------------------------------------------------------------
// FindNvidiaDriverStorePath
// ---------------------------------------------------------------------------
bool FindNvidiaDriverStorePath(wchar_t* outPath, size_t maxLen)
{
    if (!outPath || maxLen == 0) return false;
    outPath[0] = L'\0';

    // 1. Try finding NGXCore from registry (using WOW64_64KEY to ensure 64-bit view)
    wchar_t driverPath[MAX_PATH] = {};
    DWORD dataSize = sizeof(driverPath);
    LSTATUS status = RegGetValueW(
        HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore",
        L"FullPath",
        RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY,
        nullptr,
        driverPath,
        &dataSize);

    if (status == ERROR_SUCCESS && wcslen(driverPath) > 0)
    {
        wchar_t testDll[MAX_PATH] = {};
        PathCombineW(testDll, driverPath, L"_nvngx.dll");
        if (GetFileAttributesW(testDll) != INVALID_FILE_ATTRIBUTES)
        {
            DLSS_Log("[NGX] Registry NGXCore FullPath: %ls", driverPath);
            wcsncpy_s(outPath, maxLen, driverPath, _TRUNCATE);
            return true;
        }
    }

    DLSS_Log("[NGX] Registry NGXCore not found or invalid (status=%ld). Scanning DriverStore...", status);

    // 2. Fallback: Scan C:\Windows\System32\DriverStore\FileRepository\nv_dispi.inf_amd64_*
    WIN32_FIND_DATAW findData;
    HANDLE hFind = FindFirstFileW(L"C:\\Windows\\System32\\DriverStore\\FileRepository\\nv_dispi.inf_amd64_*", &findData);

    FILETIME latestTime = { 0, 0 };
    wchar_t bestPath[MAX_PATH] = {};

    if (hFind != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                wchar_t candidateDir[MAX_PATH] = {};
                swprintf_s(candidateDir, L"C:\\Windows\\System32\\DriverStore\\FileRepository\\%s", findData.cFileName);

                wchar_t candidateDll[MAX_PATH] = {};
                PathCombineW(candidateDll, candidateDir, L"_nvngx.dll");

                if (GetFileAttributesW(candidateDll) != INVALID_FILE_ATTRIBUTES)
                {
                    // Pick the directory with the most recent write time
                    if (CompareFileTime(&findData.ftLastWriteTime, &latestTime) >= 0)
                    {
                        latestTime = findData.ftLastWriteTime;
                        wcscpy_s(bestPath, candidateDir);
                    }
                }
            }
        } while (FindNextFileW(hFind, &findData));
        FindClose(hFind);
    }

    if (wcslen(bestPath) > 0)
    {
        DLSS_Log("[NGX] Found DriverStore directory: %ls", bestPath);
        wcsncpy_s(outPath, maxLen, bestPath, _TRUNCATE);
        return true;
    }

    DLSS_Log("[NGX] Warning: No valid NVIDIA DriverStore directory found.");
    return false;
}

// ---------------------------------------------------------------------------
// EnsureNGXAvailable
// ---------------------------------------------------------------------------
void EnsureNGXAvailable()
{
    wchar_t exeDir[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exeDir, MAX_PATH);
    PathRemoveFileSpecW(exeDir);

    wchar_t driverDir[MAX_PATH] = {};
    if (!FindNvidiaDriverStorePath(driverDir, MAX_PATH))
    {
        DLSS_Log("[Init] NVIDIA DriverStore dizini bulunamadi, DLL kopyalama atlandi.");
        return;
    }

    const wchar_t* dllsToCopy[] = { L"_nvngx.dll", L"nvngx.dll" };
    for (const wchar_t* dllName : dllsToCopy)
    {
        wchar_t srcPath[MAX_PATH] = {};
        wchar_t dstPath[MAX_PATH] = {};
        PathCombineW(srcPath, driverDir, dllName);
        PathCombineW(dstPath, exeDir, dllName);

        if (GetFileAttributesW(srcPath) != INVALID_FILE_ATTRIBUTES)
        {
            bool needCopy = true;
            if (GetFileAttributesW(dstPath) != INVALID_FILE_ATTRIBUTES)
            {
                WIN32_FILE_ATTRIBUTE_DATA srcAttr, dstAttr;
                if (GetFileAttributesExW(srcPath, GetFileExInfoStandard, &srcAttr) &&
                    GetFileAttributesExW(dstPath, GetFileExInfoStandard, &dstAttr))
                {
                    if (srcAttr.nFileSizeLow == dstAttr.nFileSizeLow &&
                        srcAttr.nFileSizeHigh == dstAttr.nFileSizeHigh &&
                        CompareFileTime(&srcAttr.ftLastWriteTime, &dstAttr.ftLastWriteTime) == 0)
                    {
                        needCopy = false;
                    }
                }
            }

            if (needCopy)
            {
                if (CopyFileW(srcPath, dstPath, FALSE))
                {
                    DLSS_Log("[Init] %ls DriverStore'dan uygulama klasorune basariyla kopyalandi.", dllName);
                }
                else
                {
                    DLSS_Log("[Init] %ls kopyalanamadi (LastError=%lu).", dllName, GetLastError());
                }
            }
            else
            {
                DLSS_Log("[Init] %ls zaten guncel sekilde uygulama klasorunde mevcut.", dllName);
            }
        }
        else
        {
            DLSS_Log("[Init] DriverStore'da %ls bulunamadi (%ls)", dllName, srcPath);
        }
    }
}

// ---------------------------------------------------------------------------
// LoadNGXLibrary
// ---------------------------------------------------------------------------
bool DLSSManager::LoadNGXLibrary()
{
    if (m_hNgxDll) return true;

    DLSS_Log("[NGX] Loading NVIDIA NGX Core library...");

    // 1. Try local application directory first
    wchar_t exeDir[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exeDir, MAX_PATH);
    PathRemoveFileSpecW(exeDir);

    wchar_t localDll[MAX_PATH] = {};
    PathCombineW(localDll, exeDir, L"_nvngx.dll");

    if (GetFileAttributesW(localDll) != INVALID_FILE_ATTRIBUTES)
    {
        DLSS_Log("[NGX] Attempting LoadLibrary on local: %ls", localDll);
        m_hNgxDll = LoadLibraryW(localDll);
        // Verify critical export to reject stub/fake DLLs
        if (m_hNgxDll && !GetProcAddress(m_hNgxDll, "NVSDK_NGX_D3D11_AllocateParameters"))
        {
            DLSS_Log("[NGX] Warning: Local _nvngx.dll is missing critical exports, falling back to DriverStore.");
            FreeLibrary(m_hNgxDll);
            m_hNgxDll = nullptr;
        }
    }

    // 2. Try DriverStore (Registry or dynamic DriverStore scan)
    if (!m_hNgxDll)
    {
        wchar_t driverPath[MAX_PATH] = {};
        if (FindNvidiaDriverStorePath(driverPath, MAX_PATH))
        {
            wchar_t dllFile[MAX_PATH] = {};
            PathCombineW(dllFile, driverPath, L"_nvngx.dll");
            DLSS_Log("[NGX] Attempting LoadLibrary on DriverStore: %ls", dllFile);
            m_hNgxDll = LoadLibraryW(dllFile);
            if (!m_hNgxDll)
            {
                PathCombineW(dllFile, driverPath, L"nvngx.dll");
                DLSS_Log("[NGX] Attempting fallback LoadLibrary on: %ls", dllFile);
                m_hNgxDll = LoadLibraryW(dllFile);
            }
        }
    }

    // 3. Fallback: standard LoadLibrary search
    if (!m_hNgxDll)
    {
        DLSS_Log("[NGX] Searching _nvngx.dll in application directory and PATH...");
        m_hNgxDll = LoadLibraryW(L"_nvngx.dll");
    }
    if (!m_hNgxDll)
    {
        m_hNgxDll = LoadLibraryW(L"nvngx.dll");
    }

    if (!m_hNgxDll)
    {
        DWORD err = GetLastError();
        DLSS_Log("[NGX] ERROR: Failed to load _nvngx.dll or nvngx.dll! GetLastError=0x%08X (%lu)", err, err);
        return false;
    }

    DLSS_Log("[NGX] SUCCESS: NGX DLL loaded at module handle 0x%p", m_hNgxDll);

    // Get function pointers
    m_pfnInit = reinterpret_cast<PFN_NVSDK_NGX_D3D11_Init>(
        GetProcAddress(m_hNgxDll, "NVSDK_NGX_D3D11_Init"));
    m_pfnAllocParams = reinterpret_cast<PFN_NVSDK_NGX_D3D11_AllocateParameters>(
        GetProcAddress(m_hNgxDll, "NVSDK_NGX_D3D11_AllocateParameters"));
    m_pfnGetCaps = reinterpret_cast<PFN_NVSDK_NGX_D3D11_GetCapabilityParameters>(
        GetProcAddress(m_hNgxDll, "NVSDK_NGX_D3D11_GetCapabilityParameters"));
    m_pfnDestroyParams = reinterpret_cast<PFN_NVSDK_NGX_D3D11_DestroyParameters>(
        GetProcAddress(m_hNgxDll, "NVSDK_NGX_D3D11_DestroyParameters"));
    m_pfnCreateFeature = reinterpret_cast<PFN_NVSDK_NGX_D3D11_CreateFeature>(
        GetProcAddress(m_hNgxDll, "NVSDK_NGX_D3D11_CreateFeature"));
    m_pfnEvaluateFeature = reinterpret_cast<PFN_NVSDK_NGX_D3D11_EvaluateFeature>(
        GetProcAddress(m_hNgxDll, "NVSDK_NGX_D3D11_EvaluateFeature"));
    m_pfnReleaseFeature = reinterpret_cast<PFN_NVSDK_NGX_D3D11_ReleaseFeature>(
        GetProcAddress(m_hNgxDll, "NVSDK_NGX_D3D11_ReleaseFeature"));
    m_pfnShutdown = reinterpret_cast<PFN_NVSDK_NGX_D3D11_Shutdown>(
        GetProcAddress(m_hNgxDll, "NVSDK_NGX_D3D11_Shutdown"));

    DLSS_Log("[NGX] Function Pointers:\n"
             "        NVSDK_NGX_D3D11_Init:                    0x%p\n"
             "        NVSDK_NGX_D3D11_AllocateParameters:      0x%p\n"
             "        NVSDK_NGX_D3D11_GetCapabilityParameters: 0x%p\n"
             "        NVSDK_NGX_D3D11_CreateFeature:           0x%p\n"
             "        NVSDK_NGX_D3D11_EvaluateFeature:         0x%p\n"
             "        NVSDK_NGX_D3D11_ReleaseFeature:          0x%p\n"
             "        NVSDK_NGX_D3D11_Shutdown:                0x%p",
             m_pfnInit, m_pfnAllocParams, m_pfnGetCaps,
             m_pfnCreateFeature, m_pfnEvaluateFeature,
             m_pfnReleaseFeature, m_pfnShutdown);

    bool allValid = (m_pfnInit && m_pfnAllocParams && m_pfnCreateFeature && m_pfnEvaluateFeature && m_pfnReleaseFeature);
    if (!allValid)
    {
        DLSS_Log("[NGX] ERROR: One or more critical NGX functions were not found in DLL!");
    }
    return allValid;
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
bool DLSSManager::Init(ID3D11Device* device, ID3D11DeviceContext* ctx, int width, int height)
{
    Cleanup();

    m_width  = width;
    m_height = height;
    m_evaluateCount = 0;

    DLSS_Log("===============================================================================");
    DLSS_Log("[DLSS] DLSSManager::Init called for resolution %dx%d (Device=0x%p, Context=0x%p)",
        width, height, device, ctx);

    if (!LoadNGXLibrary())
    {
        DLSS_Log("[DLSS] ERROR: LoadNGXLibrary failed. DLSS will be disabled.");
        m_available = false;
        return false;
    }

    // Get current executable directory for logs
    wchar_t appDir[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, appDir, MAX_PATH);
    PathRemoveFileSpecW(appDir);
    DLSS_Log("[DLSS] Application directory: %ls", appDir);

    // Initialize NGX D3D11 runtime
    static constexpr unsigned long long kAppId = 0x100000000ULL;
    static const unsigned int kSdkVersions[] = { 0x00000015, 0x00000014, 0x00000013, 0x00000012, 0x00000010 };

    unsigned int initResult = 0;
    bool initOk = false;

    for (unsigned int ver : kSdkVersions)
    {
        DLSS_Log("[DLSS] Calling NVSDK_NGX_D3D11_Init(AppId=0x%llX, Dir='%ls', Device=0x%p, SDKVersion=0x%08X)...",
            kAppId, appDir, device, ver);

        initResult = m_pfnInit(kAppId, appDir, device, nullptr, ver);
        DLSS_Log("[DLSS] NVSDK_NGX_D3D11_Init returned: 0x%08X (%u)", initResult, initResult);

        if (initResult == 1) // 1 = NVSDK_NGX_Result_Success
        {
            DLSS_Log("[DLSS] SUCCESS: NGX D3D11 runtime initialized with SDKVersion 0x%08X!", ver);
            initOk = true;
            break;
        }
    }

    if (!initOk)
    {
        DLSS_Log("[DLSS] ERROR: NVSDK_NGX_D3D11_Init failed for all SDK versions! Last code: 0x%08X", initResult);
        m_available = false;
        return false;
    }

    // Allocate NGX parameter interface
    unsigned int allocRes = m_pfnAllocParams(&m_ngxParams);
    DLSS_Log("[DLSS] NVSDK_NGX_D3D11_AllocateParameters returned: 0x%08X, params ptr=0x%p", allocRes, m_ngxParams);

    if (allocRes != 1 || !m_ngxParams)
    {
        DLSS_Log("[DLSS] ERROR: Failed to allocate NGX parameters!");
        m_available = false;
        return false;
    }

    // Check capabilities if available
    if (m_pfnGetCaps)
    {
        NVSDK_NGX_Parameter* caps = nullptr;
        unsigned int capsRes = m_pfnGetCaps(&caps);
        if (capsRes == 1 && caps)
        {
            unsigned int ssAvail = 0;
            unsigned int needsDriver = 0;
            caps->Get("SuperSampling.Available", &ssAvail);
            caps->Get("SuperSampling.NeedsUpdatedDriver", &needsDriver);
            DLSS_Log("[DLSS] NGX Capabilities: SuperSampling.Available=%u, NeedsUpdatedDriver=%u",
                ssAvail, needsDriver);
            if (m_pfnDestroyParams)
                m_pfnDestroyParams(caps);
        }
    }

    // Create DLSS feature & output textures
    if (!CreateFeature(device, ctx, width, height))
    {
        DLSS_Log("[DLSS] ERROR: CreateFeature failed for resolution %dx%d.", width, height);
        m_available = false;
        return false;
    }

    m_available = true;
    m_reset     = true;
    DLSS_Log("[DLSS] SUCCESS: DLSS 5 Native pipeline fully initialized and ACTIVE!");
    return true;
}

// ---------------------------------------------------------------------------
// CreateFeature
// ---------------------------------------------------------------------------
bool DLSSManager::CreateFeature(ID3D11Device* device, ID3D11DeviceContext* ctx, int width, int height)
{
    ReleaseFeature();

    DLSS_Log("[DLSS] --- Creating DLSS Feature (Width: %d, Height: %d) ---", width, height);

    // 1. Create DLSS Output Texture
    // DXGI_FORMAT_R8G8B8A8_UNORM is standard for DLSS compute output and guaranteed to support UAV
    D3D11_TEXTURE2D_DESC td = {};
    td.Width          = static_cast<UINT>(width);
    td.Height         = static_cast<UINT>(height);
    td.MipLevels      = 1;
    td.ArraySize      = 1;
    td.Format         = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc     = { 1, 0 };
    td.Usage          = D3D11_USAGE_DEFAULT;
    td.BindFlags      = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS;

    HRESULT hr = device->CreateTexture2D(&td, nullptr, &m_outputTexture);
    DLSS_Log("[DLSS] CreateTexture2D (R8G8B8A8_UNORM + UAV): hr=0x%08X, tex=0x%p", hr, m_outputTexture.Get());

    if (FAILED(hr))
    {
        DLSS_Log("[DLSS] WARNING: R8G8B8A8_UNORM failed, trying B8G8R8A8_UNORM...");
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        hr = device->CreateTexture2D(&td, nullptr, &m_outputTexture);
        DLSS_Log("[DLSS] CreateTexture2D (B8G8R8A8_UNORM): hr=0x%08X, tex=0x%p", hr, m_outputTexture.Get());
    }

    if (FAILED(hr))
    {
        DLSS_Log("[DLSS] ERROR: Could not create output texture in either format! hr=0x%08X", hr);
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvd = {};
    srvd.Format                    = td.Format;
    srvd.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvd.Texture2D.MipLevels       = 1;
    srvd.Texture2D.MostDetailedMip = 0;

    hr = device->CreateShaderResourceView(m_outputTexture.Get(), &srvd, &m_outputSRV);
    DLSS_Log("[DLSS] CreateShaderResourceView: hr=0x%08X, srv=0x%p", hr, m_outputSRV.Get());
    if (FAILED(hr)) return false;

    if (!m_ngxParams || !m_pfnCreateFeature)
    {
        DLSS_Log("[DLSS] ERROR: Missing ngxParams or pfnCreateFeature function pointer!");
        return false;
    }

    // 2. Configure DLSS creation parameters
    m_ngxParams->Set("Width", static_cast<unsigned int>(width));
    m_ngxParams->Set("Height", static_cast<unsigned int>(height));
    m_ngxParams->Set("OutWidth", static_cast<unsigned int>(width));
    m_ngxParams->Set("OutHeight", static_cast<unsigned int>(height));
    m_ngxParams->Set("PerfQualityValue", static_cast<int>(NVSDK_NGX_PerfQuality_Value_DLAA));

    unsigned int flags = NVSDK_NGX_DLSS_Feature_Flags_AutoExposure |
                         NVSDK_NGX_DLSS_Feature_Flags_DepthInverted |
                         NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
    m_ngxParams->Set("DLSS.Feature.Create.Flags", flags);

    DLSS_Log("[DLSS] Parameters set: In=%dx%d, Out=%dx%d, Quality=DLAA(5), Flags=0x%X (%u)",
        width, height, width, height, flags, flags);

    // 3. Attempt Feature 1 (SuperSampling / DLAA)
    DLSS_Log("[DLSS] Calling NVSDK_NGX_D3D11_CreateFeature (Feature ID: 1 - SuperSampling/DLAA)...");
    unsigned int res = m_pfnCreateFeature(
        ctx,
        NVSDK_NGX_Feature_SuperSampling,
        m_ngxParams,
        &m_dlssFeature);

    DLSS_Log("[DLSS] CreateFeature(1) returned: 0x%08X (%u), featureHandle=0x%p",
        res, res, m_dlssFeature ? (void*)(uintptr_t)m_dlssFeature->Id : nullptr);

    // If Feature 1 is not supported, try Feature 18 (DLSS-NR / DLSS 5 Neural Rendering)
    if (res != 1 || !m_dlssFeature)
    {
        DLSS_Log("[DLSS] Feature 1 not created. Attempting Feature ID: 18 (DLSS 5 Neural Rendering)...");
        res = m_pfnCreateFeature(
            ctx,
            NVSDK_NGX_Feature_Reserved18,
            m_ngxParams,
            &m_dlssFeature);

        DLSS_Log("[DLSS] CreateFeature(18) returned: 0x%08X (%u), featureHandle=0x%p",
            res, res, m_dlssFeature ? (void*)(uintptr_t)m_dlssFeature->Id : nullptr);
    }

    bool success = (res == 1 && m_dlssFeature != nullptr);
    if (success)
    {
        DLSS_Log("[DLSS] SUCCESS: DLSS Feature created and registered with handle!");
    }
    else
    {
        DLSS_Log("[DLSS] ERROR: CreateFeature failed with error code 0x%08X!", res);
    }
    return success;
}

// ---------------------------------------------------------------------------
// ReleaseFeature
// ---------------------------------------------------------------------------
void DLSSManager::ReleaseFeature()
{
    if (m_dlssFeature && m_pfnReleaseFeature)
    {
        DLSS_Log("[DLSS] Releasing DLSS Feature...");
        m_pfnReleaseFeature(m_dlssFeature);
        m_dlssFeature = nullptr;
        DLSS_Log("[DLSS] DLSS Feature released.");
    }

    m_outputSRV.Reset();
    m_outputTexture.Reset();
}

// ---------------------------------------------------------------------------
// Resize
// ---------------------------------------------------------------------------
void DLSSManager::Resize(ID3D11Device* device, ID3D11DeviceContext* ctx, int width, int height)
{
    DLSS_Log("[DLSS] DLSSManager::Resize to %dx%d", width, height);
    m_width  = width;
    m_height = height;
    m_reset  = true;

    if (m_available)
    {
        CreateFeature(device, ctx, width, height);
    }
}

// ---------------------------------------------------------------------------
// Evaluate
// ---------------------------------------------------------------------------
ID3D11ShaderResourceView* DLSSManager::Evaluate(
    ID3D11DeviceContext*      ctx,
    ID3D11ShaderResourceView* inputSRV,
    ID3D11Texture2D*          inputTexture,
    ID3D11Texture2D*          mvTexture,
    ID3D11Texture2D*          depthTexture,
    ID3D11Texture2D*          reactiveMaskTexture)
{
    m_evaluateCount++;

    // Fallback directly to raw input if DLSS is unavailable or disabled
    if (!m_available || !m_enabled || !m_dlssFeature || !m_ngxParams || !inputTexture)
    {
        if (m_evaluateCount <= 5)
        {
            DLSS_Log("[DLSS] Frame #%llu: DLSS bypassed (available=%d, enabled=%d, feature=0x%p, params=0x%p, inputTex=0x%p)",
                m_evaluateCount, m_available, m_enabled, m_dlssFeature, m_ngxParams, inputTexture);
        }
        return inputSRV;
    }

    // Set per-frame DLSS evaluation parameters
    m_ngxParams->Set("Color", static_cast<ID3D11Resource*>(inputTexture));
    m_ngxParams->Set("Output", static_cast<ID3D11Resource*>(m_outputTexture.Get()));

    if (mvTexture)
        m_ngxParams->Set("MotionVectors", static_cast<ID3D11Resource*>(mvTexture));
    if (depthTexture)
        m_ngxParams->Set("Depth", static_cast<ID3D11Resource*>(depthTexture));
    if (reactiveMaskTexture)
        m_ngxParams->Set("DLSS.Input.Bias.Current.Color.Mask", static_cast<ID3D11Resource*>(reactiveMaskTexture));

    m_ngxParams->Set("Reset", m_reset ? 1 : 0);
    m_ngxParams->Set("Sharpness", 0.0f);
    m_reset = false;

    // Subrect dimensions, jitter, and MV scale — REQUIRED by NVIDIA NGX for DLSS evaluation
    m_ngxParams->Set("DLSS.Render.Subrect.Dimensions.Width", static_cast<unsigned int>(m_width));
    m_ngxParams->Set("DLSS.Render.Subrect.Dimensions.Height", static_cast<unsigned int>(m_height));
    m_ngxParams->Set("Jitter.Offset.X", 0.0f);
    m_ngxParams->Set("Jitter.Offset.Y", 0.0f);
    m_ngxParams->Set("MV.Scale.X", 1.0f);
    m_ngxParams->Set("MV.Scale.Y", 1.0f);

    // Execute DLSS Neural Network
    unsigned int res = m_pfnEvaluateFeature(ctx, m_dlssFeature, m_ngxParams, nullptr);

    if (m_evaluateCount <= 10 || (m_evaluateCount % 300 == 0) || (res != 1))
    {
        DLSS_Log("[DLSS] Evaluate Frame #%llu: result=0x%08X (%s), OutputSRV=0x%p",
            m_evaluateCount, res, (res == 1) ? "SUCCESS" : "FAIL", m_outputSRV.Get());
    }

    if (res == 1) // Success
    {
        return m_outputSRV.Get();
    }

    // Fallback on evaluation error
    return inputSRV;
}

// ---------------------------------------------------------------------------
// Cleanup
// ---------------------------------------------------------------------------
void DLSSManager::Cleanup()
{
    if (!m_available && !m_hNgxDll && !m_dlssFeature && !m_ngxParams)
    {
        return;
    }

    DLSS_Log("[DLSS] DLSSManager::Cleanup called.");
    ReleaseFeature();

    if (m_ngxParams && m_pfnDestroyParams)
    {
        m_pfnDestroyParams(m_ngxParams);
        m_ngxParams = nullptr;
    }

    // NOTE: Do NOT call m_pfnShutdown() or FreeLibrary(m_hNgxDll)!
    // Calling FreeLibrary on _nvngx.dll unmaps code while NVIDIA background
    // worker threads are active, causing a fatal 0xC0000005 crash (_nvngx.dll_unloaded).
    // The driver runtime module must stay loaded for the process lifetime.

    m_available = false;
    DLSS_Log("[DLSS] DLSSManager::Cleanup completed.");
}
