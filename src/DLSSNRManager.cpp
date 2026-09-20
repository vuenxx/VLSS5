#include "DLSSNRManager.h"
#include "ConfigManager.h"
#include "DLSSManager.h"
#include <cstdio>
#include <shlwapi.h>
#pragma comment(lib, "shlwapi.lib")

bool DLSSNRManager::Init(ID3D12Device* device, ID3D12CommandQueue* queue, int width, int height)
{
    Cleanup();

    m_device = device;
    m_queue  = queue;
    m_width  = width;
    m_height = height;

    if (!m_device || !m_queue) return false;

    // Load initial settings from ConfigManager
    const auto& cfg = ConfigManager::Get().Config();
    m_preset          = cfg.preset;
    m_style           = cfg.style;
    m_intensity       = cfg.intensity;
    m_localStructure  = cfg.localStructure;
    m_localTone       = cfg.localTone;
    m_skinStructure   = cfg.skinStructure;
    m_useAutoMask     = cfg.useAutoMask ? 1 : 0;
    m_temporalStabilizer = cfg.temporalStabilizer;
    m_opticalFlow        = cfg.opticalFlow;

    m_passCount = cfg.passCount;
    if (m_passCount < 1) m_passCount = 1;
    if (m_passCount > kMaxPasses) m_passCount = kMaxPasses;

    m_passFalloff = cfg.passFalloff;
    if (m_passFalloff < 0.25f) m_passFalloff = 0.25f;
    if (m_passFalloff > 1.0f)  m_passFalloff = 1.0f;

    m_resolutionScale = cfg.resolutionScale / 100.0f;
    if (m_resolutionScale < 0.50f) m_resolutionScale = 0.50f;
    if (m_resolutionScale > 1.00f) m_resolutionScale = 1.00f;

    m_workWidth  = ((int)(m_width * m_resolutionScale + 0.5f) + 1) & ~1;
    m_workHeight = ((int)(m_height * m_resolutionScale + 0.5f) + 1) & ~1;
    if (m_workWidth < 64) m_workWidth = 64;
    if (m_workHeight < 64) m_workHeight = 64;

    // Determine paths
    wchar_t currentDir[MAX_PATH] = {};
    GetCurrentDirectoryW(MAX_PATH, currentDir);
    m_dataPath = currentDir;

    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(exePath, L'\\');
    if (lastSlash) *(lastSlash + 1) = L'\0';

    std::wstring appDir = exePath;
    m_snippetPath = appDir + L"nvngx_dlssnr.dll";

    // 1. Load NGX Core
    if (!LoadNGXCore())
    {
        DLSS_Log("[DLSS-NR] ERROR: Failed to load NGX D3D12 Core!");
        return false;
    }

    // 2. Load Forwarder Shim
    if (!LoadForwarder())
    {
        DLSS_Log("[DLSS-NR] ERROR: Failed to load nvngx.dll_dlssnr.dll forwarder shim!");
        return false;
    }

    // 2.1 Dynamically discover and register the float setter vtable slot
    DiscoverAndSetFloatSlot();

    // 3. Create Guide Textures (Depth, Motion Vectors) at Model Work Resolution
    if (!CreateGuideTextures(m_workWidth, m_workHeight))
    {
        DLSS_Log("[DLSS-NR] ERROR: Failed to create guide textures!");
        return false;
    }

    // 4. Create Feature 18
    if (!CreateFeature())
    {
        DLSS_Log("[DLSS-NR] ERROR: Failed to create Feature 18!");
        return false;
    }

    DLSS_Log("[DLSS-NR] DLSS 5 Neural Rendering initialized successfully (Display: %dx%d, Model Work: %dx%d, Scale: %.0f%%)!",
        m_width, m_height, m_workWidth, m_workHeight, m_resolutionScale * 100.0f);
    return true;
}

bool DLSSNRManager::LoadNGXCore()
{
    if (m_hNgxCore && m_params) return true;

    // 1. Try local application directory first
    wchar_t exeDir[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exeDir, MAX_PATH);
    PathRemoveFileSpecW(exeDir);

    wchar_t localDll[MAX_PATH] = {};
    PathCombineW(localDll, exeDir, L"_nvngx.dll");

    if (GetFileAttributesW(localDll) != INVALID_FILE_ATTRIBUTES)
    {
        DLSS_Log("[DLSS-NR] Loading NGX Core from local: %ls", localDll);
        m_hNgxCore = LoadLibraryW(localDll);
        if (m_hNgxCore && !GetProcAddress(m_hNgxCore, "NVSDK_NGX_D3D12_Init_Ext"))
        {
            DLSS_Log("[DLSS-NR] Warning: Local _nvngx.dll is missing D3D12 entry points, falling back to DriverStore.");
            FreeLibrary(m_hNgxCore);
            m_hNgxCore = nullptr;
        }
    }

    // 2. Try DriverStore (Registry or dynamic scan)
    if (!m_hNgxCore)
    {
        wchar_t driverPath[MAX_PATH] = {};
        if (FindNvidiaDriverStorePath(driverPath, MAX_PATH))
        {
            wchar_t ngxDllPath[MAX_PATH] = {};
            PathCombineW(ngxDllPath, driverPath, L"_nvngx.dll");
            DLSS_Log("[DLSS-NR] Loading NGX Core from DriverStore: %ls", ngxDllPath);
            m_hNgxCore = LoadLibraryW(ngxDllPath);
        }
    }

    // 3. Fallback: standard LoadLibrary search
    if (!m_hNgxCore)
    {
        DLSS_Log("[DLSS-NR] Searching _nvngx.dll via standard LoadLibrary...");
        m_hNgxCore = LoadLibraryW(L"_nvngx.dll");
    }

    if (!m_hNgxCore)
    {
        DLSS_Log("[DLSS-NR] ERROR: Cannot load _nvngx.dll! LastError=%lu", GetLastError());
        return false;
    }

    m_pfnInitExt = (PFN_NVSDK_NGX_D3D12_Init_Ext)GetProcAddress(m_hNgxCore, "NVSDK_NGX_D3D12_Init_Ext");
    m_pfnGetCaps = (PFN_NVSDK_NGX_D3D12_GetCapabilityParameters)GetProcAddress(m_hNgxCore, "NVSDK_NGX_D3D12_GetCapabilityParameters");
    m_pfnAllocParams = (PFN_NVSDK_NGX_D3D12_AllocateParameters)GetProcAddress(m_hNgxCore, "NVSDK_NGX_D3D12_AllocateParameters");
    m_pfnDestroyParams = (PFN_NVSDK_NGX_D3D12_DestroyParameters)GetProcAddress(m_hNgxCore, "NVSDK_NGX_D3D12_DestroyParameters");

    if (!m_pfnInitExt || !m_pfnGetCaps)
    {
        DLSS_Log("[DLSS-NR] ERROR: Missing required NGX D3D12 entry points!");
        return false;
    }

    int initRes = m_pfnInitExt(0x24480451ull, m_dataPath.c_str(), m_device, 0x0000015, nullptr);
    DLSS_Log("[DLSS-NR] NVSDK_NGX_D3D12_Init_Ext returned: 0x%08X", initRes);

    int capRes = m_pfnGetCaps(&m_params);
    DLSS_Log("[DLSS-NR] GetCapabilityParameters returned: 0x%08X, params=%p", capRes, m_params);
    if (!m_params && m_pfnAllocParams)
    {
        m_pfnAllocParams(&m_params);
    }

    return (m_params != nullptr);
}

bool DLSSNRManager::LoadForwarder()
{
    if (m_hForwarder && m_pfnCreate && m_pfnEvaluate) return true;

    // Check if forwarder DLL is in app directory
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(exePath, L'\\');
    if (lastSlash) *(lastSlash + 1) = L'\0';

    std::wstring fwdPath = std::wstring(exePath) + L"nvngx.dll_dlssnr.dll";

    m_hForwarder = LoadLibraryW(fwdPath.c_str());
    if (!m_hForwarder)
    {
        m_hForwarder = LoadLibraryW(L"nvngx.dll_dlssnr.dll");
    }
    if (!m_hForwarder)
    {
        DLSS_Log("[DLSS-NR] ERROR: Failed to load %ls! LastError=%lu", fwdPath.c_str(), GetLastError());
        return false;
    }

    m_pfnCreate        = (PFN_dlssnr_call_create)GetProcAddress(m_hForwarder, "dlssnr_call_create");
    m_pfnEvaluate      = (PFN_dlssnr_call_evaluate)GetProcAddress(m_hForwarder, "dlssnr_call_evaluate");
    m_pfnRelease       = (PFN_dlssnr_call_release)GetProcAddress(m_hForwarder, "dlssnr_call_release");
    m_pfnSetFloatSlot  = (PFN_dlssnr_call_set_float_slot)GetProcAddress(m_hForwarder, "dlssnr_call_set_float_slot");
    m_pLastInit        = (int*)GetProcAddress(m_hForwarder, "dlssnr_call_last_init");
    m_pLastCreate      = (int*)GetProcAddress(m_hForwarder, "dlssnr_call_last_create");

    bool ok = (m_pfnCreate && m_pfnEvaluate && m_pfnRelease);
    DLSS_Log("[DLSS-NR] Forwarder loaded (create=%p, evaluate=%p, release=%p)",
        m_pfnCreate, m_pfnEvaluate, m_pfnRelease);
    return ok;
}

void DLSSNRManager::DiscoverAndSetFloatSlot()
{
    if (!m_params || !m_pfnSetFloatSlot) return;

    NVSDK_NGX_Parameter* ngxParams = reinterpret_cast<NVSDK_NGX_Parameter*>(m_params);
    void** vt = *reinterpret_cast<void***>(m_params);
    using PFN_SetFloat = void(__thiscall*)(void*, const char*, float);

    const float probe = 0.3125f; // exact binary float
    int discoveredSlot = -1;

    for (int slot = 0; slot < 8; ++slot)
    {
        float readBack = -999.0f;
        reinterpret_cast<PFN_SetFloat>(vt[slot])(m_params, "DLSSNR.Probe", probe);
        unsigned int res = ngxParams->Get("DLSSNR.Probe", &readBack);
        if (res == 1 && readBack == probe)
        {
            discoveredSlot = slot;
            DLSS_Log("[DLSS-NR] Discovered NGX float parameter vtable slot: %d (probe verified=%.4f)", slot, readBack);
            break;
        }
    }

    if (discoveredSlot >= 0)
    {
        m_pfnSetFloatSlot(discoveredSlot);
    }
    else
    {
        DLSS_Log("[DLSS-NR] WARNING: Failed to probe float slot dynamically, defaulting to slot 6");
        m_pfnSetFloatSlot(6);
    }
}

bool DLSSNRManager::CreateGuideTextures(int width, int height)
{
    m_depthTex.Reset();
    m_motionTex.Reset();

    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;

    // 1. Depth Texture (R32_FLOAT)
    D3D12_RESOURCE_DESC rdDepth = {};
    rdDepth.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rdDepth.Width            = width;
    rdDepth.Height           = height;
    rdDepth.DepthOrArraySize = 1;
    rdDepth.MipLevels        = 1;
    rdDepth.Format           = DXGI_FORMAT_R32_FLOAT;
    rdDepth.SampleDesc.Count = 1;
    rdDepth.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rdDepth.Flags            = D3D12_RESOURCE_FLAG_NONE;

    HRESULT hr = m_device->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &rdDepth,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        nullptr, IID_PPV_ARGS(&m_depthTex));
    if (FAILED(hr))
    {
        DLSS_Log("[DLSS-NR] ERROR: Failed to create guide depth texture: 0x%08X", hr);
        return false;
    }

    // 2. Motion Vectors Texture (R16G16_FLOAT)
    D3D12_RESOURCE_DESC rdMVec = {};
    rdMVec.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rdMVec.Width            = width;
    rdMVec.Height           = height;
    rdMVec.DepthOrArraySize = 1;
    rdMVec.MipLevels        = 1;
    rdMVec.Format           = DXGI_FORMAT_R16G16_FLOAT;
    rdMVec.SampleDesc.Count = 1;
    rdMVec.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rdMVec.Flags            = D3D12_RESOURCE_FLAG_NONE;

    hr = m_device->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &rdMVec,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        nullptr, IID_PPV_ARGS(&m_motionTex));
    if (FAILED(hr))
    {
        DLSS_Log("[DLSS-NR] ERROR: Failed to create guide motion texture: 0x%08X", hr);
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// CreateChainTextures -- multipass ping-pong dokulari.
//
// Iki doku her zaman yeter: k. gecis (k-1)&1'den okur, k&1'e yazar; son gecis
// zaten dogrudan cagiranin cikti kaynagina yazdigi icin ucuncu bir ara doku
// hicbir zaman gerekmez.
//
// Bunlar D3D11 ile PAYLASILMAZ: ara sonuclarin D3D11 tarafinda gorunmesine
// gerek yok, dolayisiyla gecis basina ek paylasim veya fence bedeli de yok.
// Multipass'in tum GPU maliyeti modelin kendi calismasidir; OptiScaler'daki gibi
// gecis basina interop senkronu odenmez.
// ---------------------------------------------------------------------------
bool DLSSNRManager::CreateChainTextures(int width, int height)
{
    if (m_chainTex[0] && m_chainTex[1] && m_chainWidth == width && m_chainHeight == height)
        return true;

    m_chainTex[0].Reset();
    m_chainTex[1].Reset();
    m_chainWidth  = 0;
    m_chainHeight = 0;

    if (width <= 0 || height <= 0 || !m_device) return false;

    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width            = width;
    rd.Height           = height;
    rd.DepthOrArraySize = 1;
    rd.MipLevels        = 1;
    // Zincir boyunca D3D12Interop'un giris/cikis dokulariyla ayni format: gecisler
    // arasinda hicbir donusum olmasin, model her adimda ayni seyi gorsun.
    rd.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
    rd.SampleDesc.Count = 1;
    rd.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    for (int i = 0; i < 2; ++i)
    {
        HRESULT hr = m_device->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_COMMON, nullptr,
            IID_PPV_ARGS(&m_chainTex[i]));
        if (FAILED(hr))
        {
            DLSS_Log("[DLSS-NR] ERROR: Multipass zincir dokusu %d olusturulamadi (%dx%d): 0x%08X",
                i, width, height, hr);
            m_chainTex[0].Reset();
            m_chainTex[1].Reset();
            return false;
        }
    }

    m_chainWidth  = width;
    m_chainHeight = height;
    DLSS_Log("[DLSS-NR] Multipass zincir dokulari hazir (%dx%d, 2 adet).", width, height);
    return true;
}

bool DLSSNRManager::CreateFeature()
{
    ReleaseFeature();

    if (!m_pfnCreate || !m_device || !m_params) return false;

    const int passes = (m_passCount < 1) ? 1 : ((m_passCount > kMaxPasses) ? kMaxPasses : m_passCount);

    // Ara zincir dokulari yalnizca birden fazla gecis varken gerekir.
    if (passes > 1 && !CreateChainTextures(m_workWidth, m_workHeight))
    {
        DLSS_Log("[DLSS-NR] ERROR: Multipass zincir dokulari olusturulamadi, tek gecise dusuluyor.");
        m_passCount = 1;
        return CreateFeature();
    }

    // Temporary Command Allocator & List for creation work
    ComPtr<ID3D12CommandAllocator> cmdAlloc;
    HRESULT hr = m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&cmdAlloc));
    if (FAILED(hr)) return false;

    ComPtr<ID3D12GraphicsCommandList> cmdList;
    hr = m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmdAlloc.Get(), nullptr, IID_PPV_ARGS(&cmdList));
    if (FAILED(hr)) return false;

    DLSS_Log("[DLSS-NR] Calling dlssnr_call_create (Snippet=%ls, Work=%dx%d, Frame=%dx%d, Scale=%.0f%%, Passes=%d)...",
        m_snippetPath.c_str(), m_workWidth, m_workHeight, m_width, m_height, m_resolutionScale * 100.0f, passes);

    // Gecis basina AYRI handle. Tek handle'i ayni kare icinde tekrar cagirmak
    // ozelligin temporal gecmisini bozar; ayri handle'larda her gecis kendi
    // sabit kaynagini takip eder ve detay karelere yayilarak gercekten birikir.
    bool allOk = true;
    for (int i = 0; i < passes; ++i)
    {
        m_features[i] = m_pfnCreate(
            m_snippetPath.c_str(),
            m_dataPath.c_str(),
            m_device,
            cmdList.Get(),
            m_params,
            m_workWidth,
            m_workHeight,
            m_preset,
            m_intensity,
            m_style,
            m_localStructure,
            m_localTone,
            m_skinStructure,
            m_useAutoMask,
            m_uiCorrection
        );

        if (!m_features[i])
        {
            DLSS_Log("[DLSS-NR] ERROR: Gecis %d icin feature olusturulamadi.", i + 1);
            allOk = false;
            break;
        }
    }

    cmdList->Close();
    ID3D12CommandList* lists[] = { cmdList.Get() };
    m_queue->ExecuteCommandLists(1, lists);

    // Fence wait for initialization work to finish on GPU
    ComPtr<ID3D12Fence> fence;
    m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    HANDLE hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    m_queue->Signal(fence.Get(), 1);
    fence->SetEventOnCompletion(1, hEvent);
    WaitForSingleObject(hEvent, 2000);
    CloseHandle(hEvent);

    int lastInit = m_pLastInit ? *m_pLastInit : 0;
    int lastCreate = m_pLastCreate ? *m_pLastCreate : 0;
    DLSS_Log("[DLSS-NR] Feature Creation result: passes=%d, handle[0]=%p, lastInit=0x%08X, lastCreate=0x%08X",
        passes, m_features[0], lastInit, lastCreate);

    // Bir gecis kurulamadiysa kurulabilenlerle devam et: 3 istenip 2 elde etmek,
    // hic islem yapmamaktan iyidir ve kullanici farki zaten ekranda gorur.
    if (!allOk)
    {
        int usable = 0;
        while (usable < passes && m_features[usable]) ++usable;
        if (usable < 1)
        {
            m_needsRebuild = false;
            return false;
        }
        DLSS_Log("[DLSS-NR] UYARI: %d gecis istendi, %d gecis kuruldu. Bununla devam ediliyor.", passes, usable);
        m_passCount = usable;
    }

    m_needsRebuild = false;
    return (m_features[0] != nullptr && lastCreate == 1);
}

void DLSSNRManager::ReleaseFeature()
{
    bool any = false;
    for (int i = 0; i < kMaxPasses; ++i)
        if (m_features[i]) { any = true; break; }

    if (!any || !m_pfnRelease) return;

    // Tek bir fence beklemesi tum handle'lari kapsar: hepsi ayni kuyruga is
    // yazdi, kuyruk bosaldiginda hicbiri kullanimda degil.
    if (m_device && m_queue)
    {
        ComPtr<ID3D12Fence> fence;
        if (SUCCEEDED(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
        {
            HANDLE hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
            m_queue->Signal(fence.Get(), 1);
            fence->SetEventOnCompletion(1, hEvent);
            WaitForSingleObject(hEvent, 1000);
            CloseHandle(hEvent);
        }
    }

    DLSS_Log("[DLSS-NR] Releasing DLSS 5 Feature 18 handles...");
    for (int i = 0; i < kMaxPasses; ++i)
    {
        if (m_features[i])
        {
            m_pfnRelease(m_features[i]);
            m_features[i] = nullptr;
        }
    }
    DLSS_Log("[DLSS-NR] DLSS 5 Feature 18 handles released.");
}

bool DLSSNRManager::Resize(int width, int height)
{
    if (m_width == width && m_height == height) return true;

    m_width  = width;
    m_height = height;
    m_workWidth  = ((int)(m_width * m_resolutionScale + 0.5f) + 1) & ~1;
    m_workHeight = ((int)(m_height * m_resolutionScale + 0.5f) + 1) & ~1;
    if (m_workWidth < 64) m_workWidth = 64;
    if (m_workHeight < 64) m_workHeight = 64;
    m_firstFrame = true;

    if (!CreateGuideTextures(m_workWidth, m_workHeight)) return false;
    // Zincir dokulari work cozunurlugunde olmak zorunda; CreateFeature da bunu
    // dogruluyor ama boyut degisiminde eskilerini burada dusurmek gerekiyor.
    m_chainTex[0].Reset();
    m_chainTex[1].Reset();
    m_chainWidth  = 0;
    m_chainHeight = 0;
    return CreateFeature();
}

void DLSSNRManager::Cleanup()
{
    if (!m_hForwarder && !m_hNgxCore && !m_features[0] && !m_params)
    {
        return;
    }

    DLSS_Log("[DLSS-NR] Cleanup called.");
    ReleaseFeature();

    m_depthTex.Reset();
    m_motionTex.Reset();
    m_chainTex[0].Reset();
    m_chainTex[1].Reset();
    m_chainWidth  = 0;
    m_chainHeight = 0;

    if (m_params && m_pfnDestroyParams)
    {
        m_pfnDestroyParams(m_params);
        m_params = nullptr;
    }

    // NOTE: Do NOT call FreeLibrary on m_hNgxCore (_nvngx.dll) or m_hForwarder!
    // NVIDIA NGX maintains driver background threads and process-wide hooks.
    // Freeing _nvngx.dll causes 0xC0000005 access violations (_nvngx.dll_unloaded).
    // Keep modules loaded for process lifetime.

    m_device = nullptr;
    m_queue  = nullptr;
    m_isEvaluating = false;
    m_lastEvalResult = 0;
    m_consecutiveFailures = 0;
    m_firstFrame = true;
    DLSS_Log("[DLSS-NR] Cleanup completed.");
}

void DLSSNRManager::SetEnabled(bool v)
{
    m_enabled = v;
    if (v)
    {
        m_firstFrame = true;
        m_consecutiveFailures = 0;
        if (m_device && m_queue)
        {
            DLSS_Log("[DLSS-NR] DLSS 5 enabled via SetEnabled: Re-creating feature to restore connection...");
            CreateFeature();
        }
    }
}



bool DLSSNRManager::Evaluate(
    ID3D12GraphicsCommandList* cmdList,
    ID3D12Resource* inputColor,
    ID3D12Resource* outputRes,
    ID3D12Resource* motionVectors)
{
    if (!m_enabled || !cmdList || !inputColor || !outputRes)
    {
        m_isEvaluating = false;
        return false;
    }

    if (m_needsRebuild)
    {
        ULONGLONG now = GetTickCount64();
        if (now - m_lastConfigChangeTime >= 400)
        {
            DLSS_Log("[DLSS-NR] Rebuilding Feature 18 (debounced): Scale=%.0f%% (Work=%dx%d), Preset=%d, Style=%d, Intense=%.2f, Struct=%.2f, Tone=%.2f, Skin=%.2f, AutoMask=%d, Passes=%d",
                m_resolutionScale * 100.0f, m_workWidth, m_workHeight, m_preset, m_style, m_intensity, m_localStructure, m_localTone, m_skinStructure, m_useAutoMask, m_passCount);
            CreateGuideTextures(m_workWidth, m_workHeight);
            CreateFeature();
            m_firstFrame = true;
            m_needsRebuild = false;
        }
    }

    if (!m_features[0] || !m_pfnEvaluate)
    {
        m_isEvaluating = false;
        return false;
    }

    // Reset semantigi tek gecisteki ile ayni: optik akis varken yalnizca ilk
    // karede 1, aksi halde her karede 1 (temporal birikim kapali).
    int reset = m_firstFrame ? 1 : 0;
    if (m_temporalStabilizer || !m_opticalFlow || !motionVectors)
        reset = 1;
    m_firstFrame = false;

    // Kac gecis gercekten calistirilabilir: istenen sayi, kurulmus handle sayisi
    // ve (>1 ise) zincir dokularinin varligi ile sinirli.
    int passes = (m_passCount < 1) ? 1 : ((m_passCount > kMaxPasses) ? kMaxPasses : m_passCount);
    while (passes > 1 && !m_features[passes - 1]) --passes;
    if (passes > 1 && (!m_chainTex[0] || !m_chainTex[1] ||
                       m_chainWidth != m_workWidth || m_chainHeight != m_workHeight))
    {
        passes = 1;
    }

    static uint64_t s_evalCount = 0;
    s_evalCount++;
    const bool verbose = (s_evalCount <= 5 || (s_evalCount % 300 == 0));

    bool ok = true;
    for (int k = 0; k < passes; ++k)
    {
        // Zincir: 0. gecis gercek girdiyi okur; sonraki her gecis bir oncekinin
        // ciktisini okur; SON gecis dogrudan cagiranin cikti kaynagina yazar.
        // Boylece zincir icin fazladan tek bir kopya bile yapilmaz.
        ID3D12Resource* src = (k == 0)          ? inputColor : m_chainTex[(k - 1) & 1].Get();
        ID3D12Resource* dst = (k == passes - 1) ? outputRes  : m_chainTex[k & 1].Get();

        // Her gecis bir oncekinin uzerine daha az ekler. Falloff 1.0'da bu kapali
        // ve davranis "her gecis tam siddet" olur; dusurmek 3-4 gecisin fazla
        // pisirmesini (asiri keskinlik, plastik ten) engeller.
        float passIntensity = m_intensity;
        for (int f = 0; f < k; ++f) passIntensity *= m_passFalloff;

        if (!EvaluateSinglePass(cmdList, m_features[k], src, dst, motionVectors, reset, passIntensity))
        {
            // Zincir kirildi: son gecis cikti kaynagina hic yazmamis olabilir,
            // o yuzden basarisizligi bildir ve cagiran ham kareye donsun.
            ok = false;
            break;
        }

        if (verbose)
        {
            DLSS_Log("[DLSS-NR] Evaluate #%llu pass %d/%d: src=%p -> dst=%p, intensity=%.3f, reset=%d",
                s_evalCount, k + 1, passes, src, dst, passIntensity, reset);
        }
    }

    if (verbose || !ok)
    {
        DLSS_Log("[DLSS-NR] Evaluate #%llu: res=0x%08X (%d), passes=%d (istenen %d), falloff=%.2f, reset=%d, optFlow=%s (mvD12=%p, cfgOptFlow=%d), workSize=%dx%d (Scale=%.0f%%), intensity=%.2f, stabilizer=%d",
            s_evalCount, m_lastEvalResult, m_lastEvalResult, passes, m_passCount, m_passFalloff, reset,
            (m_opticalFlow && motionVectors) ? "ACTIVE" : "OFF",
            motionVectors, m_opticalFlow ? 1 : 0,
            m_workWidth, m_workHeight, m_resolutionScale * 100.0f,
            m_intensity, m_temporalStabilizer ? 1 : 0);
    }

    if (ok)
    {
        m_consecutiveFailures = 0;
    }
    else
    {
        m_consecutiveFailures++;
        if (m_consecutiveFailures >= 10)
        {
            DLSS_Log("[DLSS-NR] Detected %d consecutive Evaluate failures (res=0x%08X). Triggering automatic feature rebuild...",
                m_consecutiveFailures, m_lastEvalResult);
            m_consecutiveFailures = 0;
            m_needsRebuild = true;
            m_lastConfigChangeTime = GetTickCount64() - 1000;
        }
    }

    m_isEvaluating = ok;
    return m_isEvaluating;
}

// ---------------------------------------------------------------------------
// EvaluateSinglePass -- tek bir Feature 18 degerlendirmesini komut listesine yazar.
//
// Kaynaklar giriste COMMON'dan alinip cikista COMMON'a birakilir. Bu ayni zamanda
// zincirin bir sonraki gecisi icin gereken senkronu da saglar: COMMON'a donus
// bariyeri o kaynaga yapilan UAV yazmalarinin bitmesini ve sonraki okumalar icin
// gorunur olmasini garanti eder, ayrica bir UAV bariyeri gerekmez.
// ---------------------------------------------------------------------------
bool DLSSNRManager::EvaluateSinglePass(
    ID3D12GraphicsCommandList* cmdList,
    void* feature,
    ID3D12Resource* inputColor,
    ID3D12Resource* outputRes,
    ID3D12Resource* motionVectors,
    int   reset,
    float intensity)
{
    if (!feature || !cmdList || !inputColor || !outputRes) return false;

    ID3D12Resource* activeMotion = (m_opticalFlow && motionVectors) ? motionVectors : m_motionTex.Get();

    // 1. Transition resources for DLSS-NR execution
    UINT barrierCount = 2;
    D3D12_RESOURCE_BARRIER barriersIn[3] = {};
    barriersIn[0].Type  = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriersIn[0].Transition.pResource   = inputColor;
    barriersIn[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barriersIn[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    barriersIn[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    barriersIn[1].Type  = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriersIn[1].Transition.pResource   = outputRes;
    barriersIn[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barriersIn[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barriersIn[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    if (m_opticalFlow && motionVectors)
    {
        barriersIn[2].Type  = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriersIn[2].Transition.pResource   = motionVectors;
        barriersIn[2].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        barriersIn[2].Transition.StateAfter  = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barriersIn[2].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrierCount = 3;
    }

    cmdList->ResourceBarrier(barrierCount, barriersIn);

    // 2. Evaluate Feature 18 at Model Work Resolution
    int res = m_pfnEvaluate(
        cmdList,
        feature,
        m_params,
        inputColor,
        m_depthTex.Get(),
        activeMotion,
        outputRes,
        m_workWidth,
        m_workHeight,
        m_workWidth,
        m_workHeight,
        m_depthInverted ? 1 : 0,
        reset,
        intensity,
        m_style,
        m_localStructure,
        m_localTone,
        m_skinStructure,
        m_useAutoMask,
        1.0f, 1.0f // motion scale
    );

    // 3. Transition resources back to COMMON (D3D11 paylasimi / sonraki gecis icin)
    D3D12_RESOURCE_BARRIER barriersOut[3] = {};
    barriersOut[0].Type  = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriersOut[0].Transition.pResource   = inputColor;
    barriersOut[0].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    barriersOut[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_COMMON;
    barriersOut[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    barriersOut[1].Type  = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriersOut[1].Transition.pResource   = outputRes;
    barriersOut[1].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barriersOut[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_COMMON;
    barriersOut[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    if (m_opticalFlow && motionVectors)
    {
        barriersOut[2].Type  = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriersOut[2].Transition.pResource   = motionVectors;
        barriersOut[2].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barriersOut[2].Transition.StateAfter  = D3D12_RESOURCE_STATE_COMMON;
        barriersOut[2].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }

    cmdList->ResourceBarrier(barrierCount, barriersOut);

    m_lastEvalResult = res;
    return (res == 1);
}

void DLSSNRManager::ApplyConfig(const Dlss5Config& cfg)
{
    m_temporalStabilizer = cfg.temporalStabilizer;
    m_opticalFlow        = cfg.opticalFlow;

    // Falloff yalnizca Evaluate cagrisina gecen bir carpandir; feature'i yeniden
    // kurmayi gerektirmez, o yuzden asagidaki "changed" hesabinin disinda durur
    // ve slider surukleye surukleye aninda etkisini gosterir.
    m_passFalloff = cfg.passFalloff;
    if (m_passFalloff < 0.25f) m_passFalloff = 0.25f;
    if (m_passFalloff > 1.0f)  m_passFalloff = 1.0f;

    int newPassCount = cfg.passCount;
    if (newPassCount < 1) newPassCount = 1;
    if (newPassCount > kMaxPasses) newPassCount = kMaxPasses;

    // Gecis sayisi degisirse handle kumesi yeniden kurulmak ZORUNDA: her gecisin
    // kendi temporal gecmisi var ve sayi degisince zincirin sekli de degisiyor.
    bool passCountChanged = (m_passCount != newPassCount);
    m_passCount = newPassCount;

    float newScale = cfg.resolutionScale / 100.0f;
    if (newScale < 0.50f) newScale = 0.50f;
    if (newScale > 1.00f) newScale = 1.00f;

    bool scaleChanged = (fabs(m_resolutionScale - newScale) > 0.001f);
    if (scaleChanged)
    {
        m_resolutionScale = newScale;
        m_workWidth  = ((int)(m_width * m_resolutionScale + 0.5f) + 1) & ~1;
        m_workHeight = ((int)(m_height * m_resolutionScale + 0.5f) + 1) & ~1;
        if (m_workWidth < 64) m_workWidth = 64;
        if (m_workHeight < 64) m_workHeight = 64;
    }

    bool changed = scaleChanged || passCountChanged ||
                   (m_preset != cfg.preset ||
                    m_style != cfg.style ||
                    m_intensity != cfg.intensity ||
                    m_localStructure != cfg.localStructure ||
                    m_localTone != cfg.localTone ||
                    m_skinStructure != cfg.skinStructure ||
                    m_useAutoMask != (cfg.useAutoMask ? 1 : 0));

    if (!changed) return;

    m_preset         = cfg.preset;
    m_style          = cfg.style;
    m_intensity      = cfg.intensity;
    m_localStructure = cfg.localStructure;
    m_localTone      = cfg.localTone;
    m_skinStructure  = cfg.skinStructure;
    m_useAutoMask    = cfg.useAutoMask ? 1 : 0;

    m_needsRebuild   = true;
    m_lastConfigChangeTime = GetTickCount64();
}

