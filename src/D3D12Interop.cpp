#include "D3D12Interop.h"
#include "MotionVectorManager.h"
#include "ConfigManager.h"
#include "GpuSelector.h"
#include <d3dcompiler.h>
#include <cstdio>
#include <algorithm>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

// bkz. D3D12Interop.h'deki aciklama: process-omurlu, oturumlar arasi paylasilan D3D12 cihazi/kuyruk.
ComPtr<ID3D12Device>       D3D12Interop::m_d3d12Device;
ComPtr<ID3D12CommandQueue> D3D12Interop::m_cmdQueue;
LUID                       D3D12Interop::s_deviceAdapterLuid = {};
bool                       D3D12Interop::s_deviceCrossAdapter = false;
bool                       D3D12Interop::s_deviceValid = false;

// ---------------------------------------------------------------------------
// EnsureDevice
// ---------------------------------------------------------------------------
bool D3D12Interop::EnsureDevice(IDXGIAdapter* chosenAdapter, bool crossAdapter)
{
    DXGI_ADAPTER_DESC desc = {};
    chosenAdapter->GetDesc(&desc);

    if (s_deviceValid && m_d3d12Device && m_cmdQueue &&
        s_deviceCrossAdapter == crossAdapter &&
        memcmp(&s_deviceAdapterLuid, &desc.AdapterLuid, sizeof(LUID)) == 0)
    {
        DLSS_Log("[D3D12Interop] Mevcut D3D12 cihazi/komut kuyrugu yeniden kullaniliyor ('%ls').", desc.Description);
        return true;
    }

    // Hedef farkli (ya da ilk cagri): eskisini birak, sifirdan kur.
    m_cmdQueue.Reset();
    m_d3d12Device.Reset();
    s_deviceValid = false;

    HRESULT hr = D3D12CreateDevice(chosenAdapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_d3d12Device));
    if (FAILED(hr)) return false;

    D3D12_COMMAND_QUEUE_DESC qDesc = {};
    qDesc.Type     = D3D12_COMMAND_LIST_TYPE_DIRECT;
    qDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;

    D3D12_FEATURE_DATA_COMMAND_QUEUE_PRIORITY queuePriority = {};
    queuePriority.CommandListType = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queuePriority.Priority        = D3D12_COMMAND_QUEUE_PRIORITY_GLOBAL_REALTIME;

    if (SUCCEEDED(m_d3d12Device->CheckFeatureSupport(
            D3D12_FEATURE_COMMAND_QUEUE_PRIORITY,
            &queuePriority,
            sizeof(queuePriority))) &&
        queuePriority.PriorityForTypeIsSupported)
    {
        qDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_GLOBAL_REALTIME;
    }

    hr = m_d3d12Device->CreateCommandQueue(&qDesc, IID_PPV_ARGS(&m_cmdQueue));
    if (FAILED(hr) && qDesc.Priority == D3D12_COMMAND_QUEUE_PRIORITY_GLOBAL_REALTIME)
    {
        DLSS_Log("[D3D12Interop] GLOBAL_REALTIME queue creation failed (0x%08X), falling back to HIGH priority.", hr);
        qDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
        hr = m_d3d12Device->CreateCommandQueue(&qDesc, IID_PPV_ARGS(&m_cmdQueue));
    }

    if (FAILED(hr))
    {
        DLSS_Log("[D3D12Interop] ERROR: CreateCommandQueue failed: 0x%08X", hr);
        m_d3d12Device.Reset();
        return false;
    }

    DLSS_Log("[D3D12Interop] Command queue created with %s priority.",
             (qDesc.Priority == D3D12_COMMAND_QUEUE_PRIORITY_GLOBAL_REALTIME) ? "GLOBAL_REALTIME" : "HIGH");

    s_deviceAdapterLuid  = desc.AdapterLuid;
    s_deviceCrossAdapter = crossAdapter;
    s_deviceValid        = true;
    return true;
}

bool D3D12Interop::Init(ID3D11Device* d3d11Dev, ID3D11DeviceContext* d3d11Ctx, int width, int height, int workWidth, int workHeight)
{
    Cleanup();

    m_d3d11Dev = d3d11Dev;
    m_d3d11Ctx = d3d11Ctx;
    m_width    = width;
    m_height   = height;

    if (workWidth <= 0)  workWidth = width;
    if (workHeight <= 0) workHeight = height;
    m_workWidth  = workWidth;
    m_workHeight = workHeight;

    if (!m_d3d11Dev || !m_d3d11Ctx) return false;

    // Query Device5 and Context4 for D3D11 Fence support
    if (FAILED(m_d3d11Dev.As(&m_d3d11Dev5)))
    {
        DLSS_Log("[D3D12Interop] ERROR: Failed to query ID3D11Device5!");
        return false;
    }
    if (FAILED(m_d3d11Ctx.As(&m_d3d11Ctx4)))
    {
        DLSS_Log("[D3D12Interop] ERROR: Failed to query ID3D11DeviceContext4!");
        return false;
    }

    // ----------------------------------------------------------------------
    // Adapter secimi: yakalama/sunum GPU'su (A) D3D11 cihazindan gelir,
    // DLSS GPU'su (B) konfigurasyondan cozulur. Ikisi farkliysa cross-adapter
    // kopru modu devreye girer.
    // ----------------------------------------------------------------------
    ComPtr<IDXGIDevice> dxgiDev;
    if (FAILED(m_d3d11Dev.As(&dxgiDev))) return false;

    ComPtr<IDXGIAdapter> captureAdapter;
    if (FAILED(dxgiDev->GetAdapter(&captureAdapter))) return false;

    DXGI_ADAPTER_DESC captureDesc{};
    captureAdapter->GetDesc(&captureDesc);
    m_captureGpuName = captureDesc.Description;

    const std::wstring& dlssGpuPref = ConfigManager::Get().Config().dlssGpu;
    ComPtr<IDXGIAdapter1> dlssAdapter = GpuSelector::FindAdapter(dlssGpuPref);

    ComPtr<IDXGIAdapter> chosenAdapter = captureAdapter;
    m_crossAdapter = false;

    if (dlssAdapter)
    {
        DXGI_ADAPTER_DESC1 dlssDesc{};
        dlssAdapter->GetDesc1(&dlssDesc);
        if (!GpuSelector::SameAdapter(dlssDesc.AdapterLuid, captureDesc.AdapterLuid))
        {
            chosenAdapter  = dlssAdapter;
            m_crossAdapter = true;
        }
    }

    DXGI_ADAPTER_DESC chosenDesc{};
    chosenAdapter->GetDesc(&chosenDesc);
    m_dlssGpuName = chosenDesc.Description;

    HRESULT hr = S_OK;
    if (!EnsureDevice(chosenAdapter.Get(), m_crossAdapter) && m_crossAdapter)
    {
        // Secilen DLSS karti D3D12 destekleyemiyor: tek-adapter moduna geri don.
        DLSS_Log("[D3D12Interop] UYARI: DLSS GPU'su '%ls' uzerinde D3D12CreateDevice basarisiz; "
                 "yakalama GPU'suna geri donuluyor.", m_dlssGpuName.c_str());
        m_crossAdapter = false;
        chosenAdapter  = captureAdapter;
        m_dlssGpuName  = m_captureGpuName;
        if (!EnsureDevice(chosenAdapter.Get(), m_crossAdapter))
        {
            DLSS_Log("[D3D12Interop] ERROR: D3D12CreateDevice failed.");
            return false;
        }
    }
    else if (!m_d3d12Device)
    {
        DLSS_Log("[D3D12Interop] ERROR: D3D12CreateDevice failed.");
        return false;
    }

    if (m_crossAdapter)
    {
        DLSS_Log("[D3D12Interop] CROSS-ADAPTER mod: yakalama/sunum '%ls', DLSS '%ls'.",
                 m_captureGpuName.c_str(), m_dlssGpuName.c_str());
        if (!InitCrossAdapterDevice(captureAdapter.Get()))
        {
            // Kopru kurulamadi; tek-adapter moda dus ve D3D12 cihazini A'da yeniden yarat.
            DLSS_Log("[D3D12Interop] UYARI: Cross-adapter kopru kurulamadi; tek-adapter moda geri donuluyor.");
            CleanupCrossAdapter();
            m_crossAdapter = false;
            m_dlssGpuName = m_captureGpuName;
            if (!EnsureDevice(captureAdapter.Get(), m_crossAdapter))
            {
                DLSS_Log("[D3D12Interop] ERROR: D3D12CreateDevice (geri donus) failed.");
                return false;
            }
        }
    }
    else
    {
        DLSS_Log("[D3D12Interop] Tek-adapter mod: D3D12 cihazi '%ls' uzerinde.", m_dlssGpuName.c_str());
    }

    // --- GPU zaman damgasi altyapisi (teshis) ---
    if (FAILED(m_cmdQueue->GetTimestampFrequency(&m_tsFrequency)))
        m_tsFrequency = 0;

    if (m_tsFrequency)
    {
        D3D12_QUERY_HEAP_DESC qhd = {};
        qhd.Type  = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        qhd.Count = kCmdAllocCount * 2;
        if (FAILED(m_d3d12Device->CreateQueryHeap(&qhd, IID_PPV_ARGS(&m_tsHeap))))
        {
            m_tsHeap.Reset();
        }
        else
        {
            D3D12_HEAP_PROPERTIES rbProps = {};
            rbProps.Type = D3D12_HEAP_TYPE_READBACK;

            D3D12_RESOURCE_DESC rbDesc = {};
            rbDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
            rbDesc.Width            = kCmdAllocCount * 2 * sizeof(UINT64);
            rbDesc.Height           = 1;
            rbDesc.DepthOrArraySize = 1;
            rbDesc.MipLevels        = 1;
            rbDesc.SampleDesc.Count = 1;
            rbDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

            if (FAILED(m_d3d12Device->CreateCommittedResource(
                    &rbProps, D3D12_HEAP_FLAG_NONE, &rbDesc,
                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                    IID_PPV_ARGS(&m_tsReadback))))
            {
                m_tsHeap.Reset();
                m_tsReadback.Reset();
            }
        }
    }

    if (!m_tsHeap)
        DLSS_Log("[D3D12Interop] NOT: GPU zaman damgasi sorgulari kullanilamiyor, gpu suresi olculemeyecek.");

    // Create Command Allocators (triple-buffered)
    for (UINT i = 0; i < kCmdAllocCount; ++i)
    {
        hr = m_d3d12Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_cmdAlloc[i]));
        if (FAILED(hr))
        {
            DLSS_Log("[D3D12Interop] ERROR: CreateCommandAllocator[%u] failed: 0x%08X", i, hr);
            return false;
        }
        m_allocFenceValue[i] = 0;
    }
    m_allocIndex = 0;

    // Create Command List
    hr = m_d3d12Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_cmdAlloc[0].Get(), nullptr, IID_PPV_ARGS(&m_cmdList));
    if (FAILED(hr))
    {
        DLSS_Log("[D3D12Interop] ERROR: CreateCommandList failed: 0x%08X", hr);
        return false;
    }
    m_cmdList->Close();

    // Create Fences
    if (!CreateFences()) return false;

    // Komut ayirici rotasyonu ve WaitForGpu icin B cihazina ait yerel fence.
    // Cross-adapter modda m_fenceOutD12 A cihazinda yasar; bu is icin kullanilamaz.
    hr = m_d3d12Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_gpuFence));
    if (FAILED(hr))
    {
        DLSS_Log("[D3D12Interop] ERROR: CreateFence (m_gpuFence) failed: 0x%08X", hr);
        return false;
    }

    if (m_crossAdapter && !CreateCrossAdapterFences())
    {
        DLSS_Log("[D3D12Interop] ERROR: Cross-adapter fence'leri olusturulamadi.");
        return false;
    }

    // Create fence completion event
    m_hFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

    // Create Downscale Pipeline (Shaders + Sampler)
    if (!CreateDownscaleResources())
    {
        DLSS_Log("[D3D12Interop] WARNING: Failed to create downscale resources; falling back to 1:1");
    }

    // Create Shared Textures at Model Work Resolution
    if (!CreateSharedTextures(m_workWidth, m_workHeight)) return false;

    DLSS_Log("[D3D12Interop] Initialized successfully for Display %dx%d, Work %dx%d.", m_width, m_height, m_workWidth, m_workHeight);
    return true;
}

bool D3D12Interop::CreateFences()
{
    // Bu fence cifti D3D11 ile paylasilir, dolayisiyla D3D11'in uzerinde
    // oldugu adapter'in D3D12 cihazinda yaratilmak zorundadir. Cross-adapter
    // modda bu A cihazidir (m_srcDevice), aksi halde tek cihaz olan B'dir.
    ID3D12Device* d11SideDevice = m_crossAdapter ? m_srcDevice.Get() : m_d3d12Device.Get();
    if (!d11SideDevice) return false;

    // Input fence: D3D11 signals -> D3D12 waits
    HRESULT hr = d11SideDevice->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&m_fenceInD12));
    if (FAILED(hr)) return false;

    HANDLE hInFence = nullptr;
    hr = d11SideDevice->CreateSharedHandle(m_fenceInD12.Get(), nullptr, GENERIC_ALL, nullptr, &hInFence);
    if (FAILED(hr)) return false;

    hr = m_d3d11Dev5->OpenSharedFence(hInFence, IID_PPV_ARGS(&m_fenceInD11));
    CloseHandle(hInFence);
    if (FAILED(hr)) return false;

    // Output fence: D3D12 signals -> D3D11 waits
    hr = d11SideDevice->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&m_fenceOutD12));
    if (FAILED(hr)) return false;

    HANDLE hOutFence = nullptr;
    hr = d11SideDevice->CreateSharedHandle(m_fenceOutD12.Get(), nullptr, GENERIC_ALL, nullptr, &hOutFence);
    if (FAILED(hr)) return false;

    hr = m_d3d11Dev5->OpenSharedFence(hOutFence, IID_PPV_ARGS(&m_fenceOutD11));
    CloseHandle(hOutFence);
    if (FAILED(hr)) return false;

    return true;
}

// ===========================================================================
// Cross-adapter kopru
//
// A = yakalama/sunum GPU'su (D3D11 cihazi burada, monitoru bu kart suruyor)
// B = DLSS GPU'su            (m_d3d12Device, sinir agi burada kosuyor)
//
// Kare akisi:
//   1. A/D3D11 : yakalanan kareyi olcekler + optik akisi hesaplar
//   2. A/D3D12 : giris ve hareket vektorlerini cross-adapter tampona kopyalar
//   3. B/D3D12 : tampondan yerel dokulara alir, DLSS'i calistirir,
//                ciktiyi cikis tamponuna kopyalar
//   4. A/D3D12 : cikis tamponunu D3D11 sunum dokusuna kopyalar
//   5. A/D3D11 : sunar
//
// Tum senkronizasyon GPU tarafinda fence'lerle yapilir; CPU hicbir adimda
// blok olmaz (yalnizca ayirici rotasyonunda, GPU geride kalmissa).
// ===========================================================================

bool D3D12Interop::InitCrossAdapterDevice(IDXGIAdapter* captureAdapter)
{
    if (!captureAdapter) return false;

    HRESULT hr = D3D12CreateDevice(captureAdapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_srcDevice));
    if (FAILED(hr))
    {
        DLSS_Log("[D3D12Interop] ERROR: Yakalama GPU'sunda D3D12CreateDevice basarisiz: 0x%08X", hr);
        return false;
    }

    // Kopya kuyrugu yerine DIRECT kuyruk: paylasimli D3D11 dokularindan
    // kopyalarken bazi suruculer COPY kuyrugunda kisitli davraniyor ve
    // yalnizca iki kopya komutu icin ayri bir kuyruk tipinin getirisi yok.
    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type     = D3D12_COMMAND_LIST_TYPE_DIRECT;
    qd.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
    hr = m_srcDevice->CreateCommandQueue(&qd, IID_PPV_ARGS(&m_srcQueue));
    if (FAILED(hr))
    {
        qd.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        hr = m_srcDevice->CreateCommandQueue(&qd, IID_PPV_ARGS(&m_srcQueue));
    }
    if (FAILED(hr))
    {
        DLSS_Log("[D3D12Interop] ERROR: Yakalama GPU'su komut kuyrugu olusturulamadi: 0x%08X", hr);
        return false;
    }

    for (UINT i = 0; i < kCmdAllocCount; ++i)
    {
        if (FAILED(m_srcDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_srcUpAlloc[i]))) ||
            FAILED(m_srcDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_srcDnAlloc[i]))))
        {
            DLSS_Log("[D3D12Interop] ERROR: Yakalama GPU'su komut ayiricilari olusturulamadi.");
            return false;
        }
        m_srcUpFenceVal[i] = 0;
        m_srcDnFenceVal[i] = 0;
    }
    m_srcUpIndex = 0;
    m_srcDnIndex = 0;

    if (FAILED(m_srcDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_srcUpAlloc[0].Get(), nullptr, IID_PPV_ARGS(&m_srcUpList))) ||
        FAILED(m_srcDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_srcDnAlloc[0].Get(), nullptr, IID_PPV_ARGS(&m_srcDnList))))
    {
        DLSS_Log("[D3D12Interop] ERROR: Yakalama GPU'su komut listeleri olusturulamadi.");
        return false;
    }
    m_srcUpList->Close();
    m_srcDnList->Close();

    hr = m_srcDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_srcFence));
    if (FAILED(hr))
    {
        DLSS_Log("[D3D12Interop] ERROR: Yakalama GPU'su fence'i olusturulamadi: 0x%08X", hr);
        return false;
    }
    m_srcFenceValue  = 0;
    m_hSrcFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

    return true;
}

bool D3D12Interop::CreateCrossAdapterFences()
{
    if (!m_srcDevice || !m_d3d12Device) return false;

    // A -> B: A'da yaratilir, B'de acilir.
    HRESULT hr = m_srcDevice->CreateFence(
        0, D3D12_FENCE_FLAG_SHARED | D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER, IID_PPV_ARGS(&m_xaFenceInA));
    if (FAILED(hr))
    {
        DLSS_Log("[D3D12Interop] ERROR: xaFenceIn olusturulamadi: 0x%08X", hr);
        return false;
    }

    HANDLE hIn = nullptr;
    hr = m_srcDevice->CreateSharedHandle(m_xaFenceInA.Get(), nullptr, GENERIC_ALL, nullptr, &hIn);
    if (FAILED(hr)) return false;
    hr = m_d3d12Device->OpenSharedHandle(hIn, IID_PPV_ARGS(&m_xaFenceInB));
    CloseHandle(hIn);
    if (FAILED(hr))
    {
        DLSS_Log("[D3D12Interop] ERROR: xaFenceIn B tarafinda acilamadi: 0x%08X", hr);
        return false;
    }

    // B -> A: B'de yaratilir, A'da acilir.
    hr = m_d3d12Device->CreateFence(
        0, D3D12_FENCE_FLAG_SHARED | D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER, IID_PPV_ARGS(&m_xaFenceOutB));
    if (FAILED(hr))
    {
        DLSS_Log("[D3D12Interop] ERROR: xaFenceOut olusturulamadi: 0x%08X", hr);
        return false;
    }

    HANDLE hOut = nullptr;
    hr = m_d3d12Device->CreateSharedHandle(m_xaFenceOutB.Get(), nullptr, GENERIC_ALL, nullptr, &hOut);
    if (FAILED(hr)) return false;
    hr = m_srcDevice->OpenSharedHandle(hOut, IID_PPV_ARGS(&m_xaFenceOutA));
    CloseHandle(hOut);
    if (FAILED(hr))
    {
        DLSS_Log("[D3D12Interop] ERROR: xaFenceOut A tarafinda acilamadi: 0x%08X", hr);
        return false;
    }

    return true;
}

bool D3D12Interop::OpenOnSrcDevice(ID3D11Texture2D* tex, ComPtr<ID3D12Resource>& out, const char* label)
{
    out.Reset();
    if (!tex || !m_srcDevice) return false;

    ComPtr<IDXGIResource1> res;
    if (FAILED(tex->QueryInterface(IID_PPV_ARGS(&res)))) return false;

    HANDLE h = nullptr;
    HRESULT hr = res->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &h);
    if (FAILED(hr) || !h) return false;

    hr = m_srcDevice->OpenSharedHandle(h, IID_PPV_ARGS(&out));
    CloseHandle(h);
    if (FAILED(hr) || !out)
    {
        DLSS_Log("[D3D12Interop] ERROR: '%s' yakalama GPU'sunun D3D12 cihazinda acilamadi: 0x%08X", label, hr);
        return false;
    }
    return true;
}

bool D3D12Interop::CreateCrossAdapterBridge(int workWidth, int workHeight)
{
    m_xaIn.Reset();
    m_xaMv.Reset();
    m_xaOut.Reset();
    m_bridgeBytesPerFrame = 0;

    if (!m_srcDevice || !m_d3d12Device) return false;

    // Tek bir cross-adapter tampon cifti kurar.
    //   creator : tamponu URETEN cihaz (heap onun belleginde yasar, yazma yerel)
    //   opener  : tamponu TUKETEN cihaz (okuma PCIe uzerinden)
    auto makeBuffer = [&](XABuffer& xb, DXGI_FORMAT fmt, int w, int h,
                          ID3D12Device* creator, ID3D12Device* opener,
                          bool creatorIsA, const char* label) -> bool
    {
        D3D12_RESOURCE_DESC texDesc = {};
        texDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texDesc.Width            = static_cast<UINT64>(w);
        texDesc.Height           = static_cast<UINT>(h);
        texDesc.DepthOrArraySize = 1;
        texDesc.MipLevels        = 1;
        texDesc.Format           = fmt;
        texDesc.SampleDesc.Count = 1;
        texDesc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;

        UINT64 totalBytes = 0;
        creator->GetCopyableFootprints(&texDesc, 0, 1, 0, &xb.fp, nullptr, nullptr, &totalBytes);

        // Ayak izi iki cihazda ayni cikmali; satir hizalamasi (256B) spesifikasyon
        // sabiti oldugu icin pratikte ayni, yine de dogrulayip vazgeciyoruz.
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fpOther = {};
        UINT64 otherBytes = 0;
        opener->GetCopyableFootprints(&texDesc, 0, 1, 0, &fpOther, nullptr, nullptr, &otherBytes);
        if (fpOther.Footprint.RowPitch != xb.fp.Footprint.RowPitch || otherBytes != totalBytes)
        {
            DLSS_Log("[D3D12Interop] ERROR: '%s' icin iki GPU farkli ayak izi bildiriyor "
                     "(pitch %u vs %u); cross-adapter kopru kurulamaz.",
                     label, xb.fp.Footprint.RowPitch, fpOther.Footprint.RowPitch);
            return false;
        }

        const UINT64 kAlign = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
        xb.sizeBytes = (totalBytes + kAlign - 1) & ~(kAlign - 1);

        D3D12_HEAP_DESC hd = {};
        hd.SizeInBytes     = xb.sizeBytes;
        hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        hd.Alignment       = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
        hd.Flags           = D3D12_HEAP_FLAG_SHARED
                           | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER;

        ComPtr<ID3D12Heap> heapCreator, heapOpener;
        HRESULT hr = creator->CreateHeap(&hd, IID_PPV_ARGS(&heapCreator));
        if (FAILED(hr))
        {
            DLSS_Log("[D3D12Interop] ERROR: '%s' cross-adapter heap'i olusturulamadi: 0x%08X "
                     "(surucu paylasimli cross-adapter heap desteklemiyor olabilir)", label, hr);
            return false;
        }

        HANDLE hHeap = nullptr;
        hr = creator->CreateSharedHandle(heapCreator.Get(), nullptr, GENERIC_ALL, nullptr, &hHeap);
        if (FAILED(hr) || !hHeap)
        {
            DLSS_Log("[D3D12Interop] ERROR: '%s' heap paylasim tanitici alinamadi: 0x%08X", label, hr);
            return false;
        }
        hr = opener->OpenSharedHandle(hHeap, IID_PPV_ARGS(&heapOpener));
        CloseHandle(hHeap);
        if (FAILED(hr) || !heapOpener)
        {
            DLSS_Log("[D3D12Interop] ERROR: '%s' heap karsi GPU'da acilamadi: 0x%08X", label, hr);
            return false;
        }

        D3D12_RESOURCE_DESC bd = {};
        bd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width            = xb.sizeBytes;
        bd.Height           = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels        = 1;
        bd.Format           = DXGI_FORMAT_UNKNOWN;
        bd.SampleDesc.Count = 1;
        bd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;

        // Iki cihaz kaynak durumunu AYRI izler; uretici tarafta hep COPY_DEST,
        // tuketici tarafta hep COPY_SOURCE kalir, gecis gerekmez.
        ComPtr<ID3D12Resource> bufCreator, bufOpener;
        hr = creator->CreatePlacedResource(heapCreator.Get(), 0, &bd,
                                           D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&bufCreator));
        if (FAILED(hr))
        {
            DLSS_Log("[D3D12Interop] ERROR: '%s' uretici tamponu olusturulamadi: 0x%08X", label, hr);
            return false;
        }
        hr = opener->CreatePlacedResource(heapOpener.Get(), 0, &bd,
                                          D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr, IID_PPV_ARGS(&bufOpener));
        if (FAILED(hr))
        {
            DLSS_Log("[D3D12Interop] ERROR: '%s' tuketici tamponu olusturulamadi: 0x%08X", label, hr);
            return false;
        }

        if (creatorIsA)
        {
            xb.heapA = heapCreator; xb.bufA = bufCreator;
            xb.heapB = heapOpener;  xb.bufB = bufOpener;
        }
        else
        {
            xb.heapB = heapCreator; xb.bufB = bufCreator;
            xb.heapA = heapOpener;  xb.bufA = bufOpener;
        }

        m_bridgeBytesPerFrame += static_cast<size_t>(totalBytes);
        return true;
    };

    ID3D12Device* A = m_srcDevice.Get();
    ID3D12Device* B = m_d3d12Device.Get();

    // Giris ve hareket vektorleri A'da uretilir, B'de tuketilir.
    if (!makeBuffer(m_xaIn,  DXGI_FORMAT_B8G8R8A8_UNORM, workWidth, workHeight, A, B, true,  "giris"))   return false;
    if (!makeBuffer(m_xaMv,  DXGI_FORMAT_R16G16_FLOAT,   workWidth, workHeight, A, B, true,  "hareket")) return false;
    // Cikis B'de uretilir, A'da tuketilir.
    if (!makeBuffer(m_xaOut, DXGI_FORMAT_B8G8R8A8_UNORM, workWidth, workHeight, B, A, false, "cikis"))   return false;

    DLSS_Log("[D3D12Interop] Cross-adapter kopru hazir: %dx%d, kare basina %.2f MB PCIe trafigi "
             "(giris + hareket + cikis).",
             workWidth, workHeight, static_cast<double>(m_bridgeBytesPerFrame) / (1024.0 * 1024.0));
    return true;
}

bool D3D12Interop::SubmitUploadToBridge()
{
    if (!m_srcQueue || !m_srcUpList || !m_srcInD12 || !m_xaIn.bufA) return false;

    // 1. A kuyrugu, D3D11'in olcekleme + optik akis isini bitirmesini bekler.
    m_srcQueue->Wait(m_fenceInD12.Get(), m_frameIndex);

    // 2. Ayirici rotasyonu: bu slotun onceki gonderimi GPU'da bitmemisse bekle.
    m_srcUpIndex = (m_srcUpIndex + 1) % kCmdAllocCount;
    const UINT64 needed = m_srcUpFenceVal[m_srcUpIndex];
    if (needed > 0 && m_srcFence->GetCompletedValue() < needed && m_hSrcFenceEvent)
    {
        m_srcFence->SetEventOnCompletion(needed, m_hSrcFenceEvent);
        if (WaitForSingleObject(m_hSrcFenceEvent, 1000) == WAIT_TIMEOUT)
            DLSS_Log("[D3D12Interop] UYARI: Yakalama GPU'su yukleme fence beklemesi zaman asimina ugradi (%llu).", needed);
    }

    m_srcUpAlloc[m_srcUpIndex]->Reset();
    m_srcUpList->Reset(m_srcUpAlloc[m_srcUpIndex].Get(), nullptr);

    // 3. D3D11 dokularindan cross-adapter tamponlara kopyala.
    // Paylasimli D3D11 kaynaklari D3D12'de COMMON'da durur; kopya kaynagi olarak
    // ortuk durum yukseltmesi (COMMON -> COPY_SOURCE) gecerlidir, bariyere gerek yok.
    {
        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource       = m_xaIn.bufA.Get();
        dst.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = m_xaIn.fp;

        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource        = m_srcInD12.Get();
        src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;

        m_srcUpList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }

    if (m_srcMvD12 && m_xaMv.bufA)
    {
        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource       = m_xaMv.bufA.Get();
        dst.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = m_xaMv.fp;

        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource        = m_srcMvD12.Get();
        src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;

        m_srcUpList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }

    m_srcUpList->Close();
    ID3D12CommandList* lists[] = { m_srcUpList.Get() };
    m_srcQueue->ExecuteCommandLists(1, lists);

    // 4. B'yi uyandiracak cross-adapter fence + yerel ayirici izleme fence'i.
    m_srcQueue->Signal(m_xaFenceInA.Get(), m_frameIndex);
    m_srcUpFenceVal[m_srcUpIndex] = ++m_srcFenceValue;
    m_srcQueue->Signal(m_srcFence.Get(), m_srcFenceValue);

    return true;
}

bool D3D12Interop::SubmitDownloadFromBridge()
{
    if (!m_srcQueue || !m_srcDnList || !m_srcOutD12 || !m_xaOut.bufA) return false;

    // 1. A kuyrugu, B'nin DLSS ciktisini cikis tamponuna yazmasini bekler.
    m_srcQueue->Wait(m_xaFenceOutA.Get(), m_frameIndex);

    // 2. Ayirici rotasyonu.
    m_srcDnIndex = (m_srcDnIndex + 1) % kCmdAllocCount;
    const UINT64 needed = m_srcDnFenceVal[m_srcDnIndex];
    if (needed > 0 && m_srcFence->GetCompletedValue() < needed && m_hSrcFenceEvent)
    {
        m_srcFence->SetEventOnCompletion(needed, m_hSrcFenceEvent);
        if (WaitForSingleObject(m_hSrcFenceEvent, 1000) == WAIT_TIMEOUT)
            DLSS_Log("[D3D12Interop] UYARI: Yakalama GPU'su indirme fence beklemesi zaman asimina ugradi (%llu).", needed);
    }

    m_srcDnAlloc[m_srcDnIndex]->Reset();
    m_srcDnList->Reset(m_srcDnAlloc[m_srcDnIndex].Get(), nullptr);

    {
        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource        = m_srcOutD12.Get();
        dst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource       = m_xaOut.bufA.Get();
        src.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = m_xaOut.fp;

        m_srcDnList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }

    m_srcDnList->Close();
    ID3D12CommandList* lists[] = { m_srcDnList.Get() };
    m_srcQueue->ExecuteCommandLists(1, lists);

    // 3. D3D11'in bekledigi cikis fence'i: artik sunum dokusu hazir.
    m_srcQueue->Signal(m_fenceOutD12.Get(), m_frameIndex);
    m_srcDnFenceVal[m_srcDnIndex] = ++m_srcFenceValue;
    m_srcQueue->Signal(m_srcFence.Get(), m_srcFenceValue);

    return true;
}

void D3D12Interop::CleanupCrossAdapter()
{
    // A kuyrugundaki isler bitmeden kaynaklari birakma.
    if (m_srcQueue && m_srcFence && m_hSrcFenceEvent)
    {
        const UINT64 v = ++m_srcFenceValue;
        m_srcQueue->Signal(m_srcFence.Get(), v);
        if (m_srcFence->GetCompletedValue() < v)
        {
            m_srcFence->SetEventOnCompletion(v, m_hSrcFenceEvent);
            WaitForSingleObject(m_hSrcFenceEvent, 2000);
        }
    }

    m_xaOut.Reset();
    m_xaMv.Reset();
    m_xaIn.Reset();

    m_xaFenceOutA.Reset();
    m_xaFenceOutB.Reset();
    m_xaFenceInB.Reset();
    m_xaFenceInA.Reset();

    m_srcOutD12.Reset();
    m_srcMvD12.Reset();
    m_srcInD12.Reset();

    m_srcDnList.Reset();
    m_srcUpList.Reset();
    for (UINT i = 0; i < kCmdAllocCount; ++i)
    {
        m_srcDnAlloc[i].Reset();
        m_srcUpAlloc[i].Reset();
        m_srcDnFenceVal[i] = 0;
        m_srcUpFenceVal[i] = 0;
    }
    m_srcUpIndex = 0;
    m_srcDnIndex = 0;

    if (m_hSrcFenceEvent)
    {
        CloseHandle(m_hSrcFenceEvent);
        m_hSrcFenceEvent = nullptr;
    }
    m_srcFence.Reset();
    m_srcFenceValue = 0;
    m_srcQueue.Reset();
    m_srcDevice.Reset();

    m_bridgeBytesPerFrame = 0;
}

bool D3D12Interop::CreateDownscaleResources()
{
    if (!m_d3d11Dev) return false;

    static const char* s_vsSource = R"HLSL(
    void VS(uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0)
    {
        float2 verts[3] = { float2(-1.0f, 1.0f), float2(3.0f, 1.0f), float2(-1.0f, -3.0f) };
        float2 uvs[3]   = { float2(0.0f, 0.0f),  float2(2.0f, 0.0f), float2(0.0f, 2.0f) };
        pos = float4(verts[id], 0.0f, 1.0f);
        uv  = uvs[id];
    }
    )HLSL";

    static const char* s_psSource = R"HLSL(
    Texture2D<float4> gSrcTex : register(t0);
    SamplerState      gLinear : register(s0);

    cbuffer DownscaleParams : register(b0)
    {
        float2 g_invDestDims; // 1 / (calisma cozunurlugu genislik,yukseklik)
        float2 g_pad;
    };

    float4 PS(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
    {
        // 4-tap jittered box filter for smooth anti-aliased proxy downsampling.
        //
        // Yaricap HEDEF (calisma cozunurlugu) piksel izdusumune gore olcekli
        // olmak ZORUNDA. Eskiden gSrcTex.GetDimensions() (native/kaynak boyut)
        // kullaniliyordu -- bu, olcek orani ne olursa olsun SABIT 1 kaynak
        // pikselik bir kutu veriyordu. %50 gibi agresif kucultmede hedefin
        // her pikseli aslinda ~2x2 kaynak pikseli temsil etmeli; sabit/dar
        // kutu bu bilgiyi es geciyor, alias/gurultu birikiyor ve NVOF optik
        // akis bu gurultulu proxy'den kestirim yaptigi icin hareket
        // vektorleri gittikce yanlis cikip golgelerin "kaymasina" yol aciyor
        // -- olcek dustukce bu etki (hedef/kaynak orani buyudukce) katlanarak
        // artiyordu. Hedef piksel izdusumune gore yaricap kullanmak (yani
        // g_invDestDims) kutuyu HER olcek oraninda doğru genislikte tutar.
        float2 halfTexel = 0.5f * g_invDestDims;

        float4 s0 = gSrcTex.Sample(gLinear, uv + float2(-halfTexel.x, -halfTexel.y));
        float4 s1 = gSrcTex.Sample(gLinear, uv + float2( halfTexel.x, -halfTexel.y));
        float4 s2 = gSrcTex.Sample(gLinear, uv + float2(-halfTexel.x,  halfTexel.y));
        float4 s3 = gSrcTex.Sample(gLinear, uv + float2( halfTexel.x,  halfTexel.y));

        return 0.25f * (s0 + s1 + s2 + s3);
    }
    )HLSL";

    ComPtr<ID3DBlob> vsBlob, psBlob, errBlob;
    HRESULT hr = D3DCompile(s_vsSource, strlen(s_vsSource), "DownscaleVS", nullptr, nullptr, "VS", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vsBlob, &errBlob);
    if (FAILED(hr)) return false;
    hr = m_d3d11Dev->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &m_downscaleVS);
    if (FAILED(hr)) return false;

    hr = D3DCompile(s_psSource, strlen(s_psSource), "DownscalePS", nullptr, nullptr, "PS", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psBlob, &errBlob);
    if (FAILED(hr)) return false;
    hr = m_d3d11Dev->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &m_downscalePS);
    if (FAILED(hr)) return false;

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter   = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD   = D3D11_FLOAT32_MAX;
    hr = m_d3d11Dev->CreateSamplerState(&sd, &m_downscaleSampler);
    if (FAILED(hr)) return false;

    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth      = 16; // float2 g_invDestDims + float2 pad, 16-byte hizali
    cbd.Usage          = D3D11_USAGE_DEFAULT;
    cbd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    hr = m_d3d11Dev->CreateBuffer(&cbd, nullptr, &m_downscaleCB);
    return SUCCEEDED(hr);
}

// Cross-adapter modda DLSS GPU'sunda (B) yasayan yerel doku. Cross-adapter
// tampondan buraya kopyalanir; DLSS her zaman yerel, optimal yerlesimli bir
// kaynak okur/yazar.
static bool CreateLocalTexture(ID3D12Device* dev, DXGI_FORMAT fmt, int w, int h,
                               bool allowUAV, ComPtr<ID3D12Resource>& out, const char* label)
{
    out.Reset();
    if (!dev) return false;

    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width            = static_cast<UINT64>(w);
    rd.Height           = static_cast<UINT>(h);
    rd.DepthOrArraySize = 1;
    rd.MipLevels        = 1;
    rd.Format           = fmt;
    rd.SampleDesc.Count = 1;
    rd.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags            = allowUAV ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
                                   : D3D12_RESOURCE_FLAG_NONE;

    HRESULT hr = dev->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&out));
    if (FAILED(hr))
    {
        DLSS_Log("[D3D12Interop] ERROR: DLSS GPU'sunda '%s' yerel dokusu olusturulamadi: 0x%08X", label, hr);
        return false;
    }
    return true;
}

bool D3D12Interop::CreateSharedTextures(int workWidth, int workHeight)
{
    m_sharedInD11.Reset();
    m_sharedInRTV.Reset();
    m_sharedInD12.Reset();
    m_sharedOutD11.Reset();
    m_sharedOutSRV.Reset();
    m_sharedOutD12.Reset();
    m_nativeOutD12.Reset();
    m_srcInD12.Reset();
    m_srcMvD12.Reset();
    m_srcOutD12.Reset();

    m_workWidth  = workWidth;
    m_workHeight = workHeight;

    // 1. Shared Input Texture (Receives captured/downscaled frame in D3D11, read by D3D12 Feature 18)
    D3D11_TEXTURE2D_DESC tdIn = {};
    tdIn.Width     = workWidth;
    tdIn.Height    = workHeight;
    tdIn.MipLevels = 1;
    tdIn.ArraySize = 1;
    tdIn.Format    = DXGI_FORMAT_B8G8R8A8_UNORM;
    tdIn.SampleDesc.Count = 1;
    tdIn.Usage     = D3D11_USAGE_DEFAULT;
    tdIn.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    tdIn.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;

    HRESULT hr = m_d3d11Dev->CreateTexture2D(&tdIn, nullptr, &m_sharedInD11);
    if (FAILED(hr))
    {
        DLSS_Log("[D3D12Interop] ERROR: Failed to create m_sharedInD11 (%dx%d): 0x%08X", workWidth, workHeight, hr);
        return false;
    }

    hr = m_d3d11Dev->CreateRenderTargetView(m_sharedInD11.Get(), nullptr, &m_sharedInRTV);
    if (FAILED(hr))
    {
        DLSS_Log("[D3D12Interop] ERROR: Failed to create m_sharedInRTV: 0x%08X", hr);
        return false;
    }

    hr = m_d3d11Dev->CreateShaderResourceView(m_sharedInD11.Get(), nullptr, &m_sharedInSRV);
    if (FAILED(hr))
    {
        DLSS_Log("[D3D12Interop] ERROR: Failed to create m_sharedInSRV: 0x%08X", hr);
        return false;
    }

    if (m_crossAdapter)
    {
        // A tarafi: D3D11 dokusunu A'nin D3D12 cihazinda ac (kopya kaynagi).
        if (!OpenOnSrcDevice(m_sharedInD11.Get(), m_srcInD12, "giris")) return false;
        // B tarafi: DLSS'in okuyacagi yerel doku.
        if (!CreateLocalTexture(m_d3d12Device.Get(), DXGI_FORMAT_B8G8R8A8_UNORM,
                                workWidth, workHeight, false, m_sharedInD12, "giris")) return false;
    }
    else
    {
        ComPtr<IDXGIResource1> resIn;
        hr = m_sharedInD11.As(&resIn);
        if (FAILED(hr)) return false;

        HANDLE hSharedIn = nullptr;
        hr = resIn->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &hSharedIn);
        if (FAILED(hr) || !hSharedIn) return false;

        hr = m_d3d12Device->OpenSharedHandle(hSharedIn, IID_PPV_ARGS(&m_sharedInD12));
        CloseHandle(hSharedIn);
        if (FAILED(hr) || !m_sharedInD12)
        {
            DLSS_Log("[D3D12Interop] ERROR: Failed to open m_sharedInD12: 0x%08X", hr);
            return false;
        }
    }

    // 2. Shared Output Texture (Direct Zero-Copy UAV or fallback)
    m_useDirectSharedOut = false;
    D3D11_TEXTURE2D_DESC tdOut = {};
    tdOut.Width     = workWidth;
    tdOut.Height    = workHeight;
    tdOut.MipLevels = 1;
    tdOut.ArraySize = 1;
    tdOut.Format    = DXGI_FORMAT_B8G8R8A8_UNORM;
    tdOut.SampleDesc.Count = 1;
    tdOut.Usage     = D3D11_USAGE_DEFAULT;
    // Cross-adapter modda DLSS zaten B'deki yerel dokuya yazar ve sonuc kopru
    // uzerinden gelir; zero-copy UAV paylasimi anlamsiz, denemiyoruz bile.
    tdOut.BindFlags = m_crossAdapter
        ? (D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET)
        : (D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS);
    tdOut.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;

    hr = m_d3d11Dev->CreateTexture2D(&tdOut, nullptr, &m_sharedOutD11);
    if (SUCCEEDED(hr) && !m_crossAdapter)
    {
        m_useDirectSharedOut = true;
    }
    else if (FAILED(hr))
    {
        // Fallback for drivers/adapters that don't support UAV on shared B8G8R8A8
        DLSS_Log("[D3D12Interop] Note: Direct shared UAV output texture unsupported (0x%08X), using native copy fallback.", hr);
        tdOut.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        hr = m_d3d11Dev->CreateTexture2D(&tdOut, nullptr, &m_sharedOutD11);
        if (FAILED(hr))
        {
            DLSS_Log("[D3D12Interop] ERROR: Failed to create m_sharedOutD11: 0x%08X", hr);
            return false;
        }
    }

    hr = m_d3d11Dev->CreateShaderResourceView(m_sharedOutD11.Get(), nullptr, &m_sharedOutSRV);
    if (FAILED(hr))
    {
        DLSS_Log("[D3D12Interop] ERROR: Failed to create m_sharedOutSRV: 0x%08X", hr);
        return false;
    }

    if (m_crossAdapter)
    {
        // A tarafi: sunum dokusunu A'nin D3D12 cihazinda ac (kopya hedefi).
        if (!OpenOnSrcDevice(m_sharedOutD11.Get(), m_srcOutD12, "cikis")) return false;
    }
    else
    {
        ComPtr<IDXGIResource1> resOut;
        hr = m_sharedOutD11.As(&resOut);
        if (FAILED(hr)) return false;

        HANDLE hSharedOut = nullptr;
        hr = resOut->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &hSharedOut);
        if (FAILED(hr) || !hSharedOut) return false;

        hr = m_d3d12Device->OpenSharedHandle(hSharedOut, IID_PPV_ARGS(&m_sharedOutD12));
        CloseHandle(hSharedOut);
        if (FAILED(hr) || !m_sharedOutD12)
        {
            DLSS_Log("[D3D12Interop] ERROR: Failed to open m_sharedOutD12: 0x%08X", hr);
            return false;
        }
    }

    if (m_useDirectSharedOut)
    {
        DLSS_Log("[D3D12Interop] Zero-Copy Output Texture active (redundant D3D12 copy eliminated)!");
    }
    else
    {
        // Fallback: Native D3D12 Output Texture with FULL UAV support (for DLSS-NR neural evaluation)
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width            = workWidth;
        rd.Height           = workHeight;
        rd.DepthOrArraySize = 1;
        rd.MipLevels        = 1;
        rd.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
        rd.SampleDesc.Count = 1;
        rd.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        hr = m_d3d12Device->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_COMMON, nullptr,
            IID_PPV_ARGS(&m_nativeOutD12));
        if (FAILED(hr))
        {
            DLSS_Log("[D3D12Interop] ERROR: Failed to create m_nativeOutD12 fallback: 0x%08X", hr);
            return false;
        }
    }

    // 4. Shared Motion Vectors Texture (R16G16_FLOAT) for Optical Flow
    D3D11_TEXTURE2D_DESC tdMv = {};
    tdMv.Width     = workWidth;
    tdMv.Height    = workHeight;
    tdMv.MipLevels = 1;
    tdMv.ArraySize = 1;
    tdMv.Format    = DXGI_FORMAT_R16G16_FLOAT;
    tdMv.SampleDesc.Count = 1;
    tdMv.Usage     = D3D11_USAGE_DEFAULT;
    tdMv.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    tdMv.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;

    hr = m_d3d11Dev->CreateTexture2D(&tdMv, nullptr, &m_sharedMvD11);
    if (FAILED(hr))
    {
        DLSS_Log("[D3D12Interop] ERROR: Failed to create m_sharedMvD11 (%dx%d): 0x%08X", workWidth, workHeight, hr);
        return false;
    }

    hr = m_d3d11Dev->CreateUnorderedAccessView(m_sharedMvD11.Get(), nullptr, &m_sharedMvUAV);
    if (FAILED(hr))
    {
        DLSS_Log("[D3D12Interop] ERROR: Failed to create m_sharedMvUAV: 0x%08X", hr);
        return false;
    }

    hr = m_d3d11Dev->CreateShaderResourceView(m_sharedMvD11.Get(), nullptr, &m_sharedMvSRV);
    if (FAILED(hr))
    {
        DLSS_Log("[D3D12Interop] ERROR: Failed to create m_sharedMvSRV: 0x%08X", hr);
        return false;
    }

    if (m_crossAdapter)
    {
        if (!OpenOnSrcDevice(m_sharedMvD11.Get(), m_srcMvD12, "hareket")) return false;
        if (!CreateLocalTexture(m_d3d12Device.Get(), DXGI_FORMAT_R16G16_FLOAT,
                                workWidth, workHeight, false, m_sharedMvD12, "hareket")) return false;
    }
    else
    {
        ComPtr<IDXGIResource1> resMv;
        hr = m_sharedMvD11.As(&resMv);
        if (FAILED(hr)) return false;

        HANDLE hSharedMv = nullptr;
        hr = resMv->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &hSharedMv);
        if (FAILED(hr) || !hSharedMv) return false;

        hr = m_d3d12Device->OpenSharedHandle(hSharedMv, IID_PPV_ARGS(&m_sharedMvD12));
        CloseHandle(hSharedMv);
        if (FAILED(hr) || !m_sharedMvD12)
        {
            DLSS_Log("[D3D12Interop] ERROR: Failed to open m_sharedMvD12: 0x%08X", hr);
            return false;
        }
    }

    // 5. Cross-adapter tamponlari calisma cozunurluguyle birlikte yeniden kurulur.
    if (m_crossAdapter && !CreateCrossAdapterBridge(workWidth, workHeight))
        return false;

    return true;
}

void D3D12Interop::WaitForGpu()
{
    // Cross-adapter modda once A kuyrugu bosaltilir: A'nin bekledigi fence'ler
    // B tarafindan sinyallenir, ters sirada beklersek kilitlenebiliriz.
    if (m_crossAdapter && m_srcQueue && m_srcFence && m_hSrcFenceEvent)
    {
        const UINT64 v = ++m_srcFenceValue;
        m_srcQueue->Signal(m_srcFence.Get(), v);
        if (m_srcFence->GetCompletedValue() < v)
        {
            m_srcFence->SetEventOnCompletion(v, m_hSrcFenceEvent);
            WaitForSingleObject(m_hSrcFenceEvent, 2000);
        }
    }

    if (!m_cmdQueue || !m_d3d12Device) return;
    ComPtr<ID3D12Fence> fence;
    if (SUCCEEDED(m_d3d12Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
    {
        HANDLE hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (hEvent)
        {
            m_cmdQueue->Signal(fence.Get(), 1);
            fence->SetEventOnCompletion(1, hEvent);
            WaitForSingleObject(hEvent, 2000);
            CloseHandle(hEvent);
        }
    }
}

bool D3D12Interop::ResizeWork(int workWidth, int workHeight)
{
    if (m_workWidth == workWidth && m_workHeight == workHeight && m_sharedInD11) return true;
    DLSS_Log("[D3D12Interop] Resizing work textures: %dx%d -> %dx%d (Display: %dx%d)",
        m_workWidth, m_workHeight, workWidth, workHeight, m_width, m_height);
    WaitForGpu();
    return CreateSharedTextures(workWidth, workHeight);
}

bool D3D12Interop::Resize(int width, int height, int workWidth, int workHeight)
{
    if (workWidth <= 0)  workWidth = width;
    if (workHeight <= 0) workHeight = height;

    if (m_width == width && m_height == height && m_workWidth == workWidth && m_workHeight == workHeight) return true;

    m_width  = width;
    m_height = height;
    WaitForGpu();
    return CreateSharedTextures(workWidth, workHeight);
}

void D3D12Interop::Cleanup()
{
    WaitForGpu();

    CleanupCrossAdapter();
    m_crossAdapter = false;
    m_gpuFence.Reset();
    m_gpuFenceValue = 0;

    m_tsHeap.Reset();
    m_tsReadback.Reset();
    m_diagStagingIn.Reset();
    m_diagStagingOut.Reset();

    if (m_hFenceEvent)
    {
        CloseHandle(m_hFenceEvent);
        m_hFenceEvent = nullptr;
    }

    m_nativeOutD12.Reset();
    m_sharedMvD12.Reset();
    m_sharedMvSRV.Reset();
    m_sharedMvUAV.Reset();
    m_sharedMvD11.Reset();
    m_sharedInD12.Reset();
    m_sharedInSRV.Reset();
    m_sharedInRTV.Reset();
    m_sharedInD11.Reset();
    m_sharedOutD12.Reset();
    m_sharedOutSRV.Reset();
    m_sharedOutD11.Reset();

    m_downscaleVS.Reset();
    m_downscalePS.Reset();
    m_downscaleSampler.Reset();
    m_downscaleCB.Reset();

    m_fenceInD11.Reset();
    m_fenceInD12.Reset();
    m_fenceOutD11.Reset();
    m_fenceOutD12.Reset();

    m_cmdList.Reset();
    for (UINT i = 0; i < kCmdAllocCount; ++i)
    {
        m_cmdAlloc[i].Reset();
        m_allocFenceValue[i] = 0;
    }
    m_allocIndex = 0;
    // m_cmdQueue/m_d3d12Device KASITLI OLARAK sifirlanmiyor -- static,
    // process-omurlu (bkz. header aciklamasi). Bir sonraki Init() ayni
    // adapter/moda ihtiyac duyarsa EnsureDevice bunlari oldugu gibi kullanir.

    m_d3d11Ctx4.Reset();
    m_d3d11Dev5.Reset();
    m_d3d11Ctx.Reset();
    m_d3d11Dev.Reset();

    m_frameIndex = 0;
}

bool D3D12Interop::BeginFrame(
    ID3D11Texture2D* srcCapturedTex,
    ID3D11ShaderResourceView* srcCapturedSRV,
    MotionVectorManager* mvMgr)
{
    if (!srcCapturedTex || !m_sharedInD11 || !m_cmdList || !m_d3d11Ctx4) return false;

    m_frameIndex++;

    // 1. Copy or downscale captured frame into shared input texture in D3D11
    if (m_workWidth == m_width && m_workHeight == m_height)
    {
        D3D11_TEXTURE2D_DESC srcDesc = {};
        srcCapturedTex->GetDesc(&srcDesc);
        if (srcDesc.Width != (UINT)m_width || srcDesc.Height != (UINT)m_height)
        {
            static bool s_sizeWarnLogged = false;
            if (!s_sizeWarnLogged)
            {
                s_sizeWarnLogged = true;
                DLSS_Log("[D3D12Interop] WARNING: srcCapturedTex size %ux%u != interop size %dx%d. Using subresource box copy.",
                    srcDesc.Width, srcDesc.Height, m_width, m_height);
            }
            D3D11_BOX box = {};
            box.left   = 0;
            box.top    = 0;
            box.front  = 0;
            box.right  = std::min((UINT)m_width, srcDesc.Width);
            box.bottom = std::min((UINT)m_height, srcDesc.Height);
            box.back   = 1;
            m_d3d11Ctx->CopySubresourceRegion(m_sharedInD11.Get(), 0, 0, 0, 0, srcCapturedTex, 0, &box);
        }
        else
        {
            m_d3d11Ctx->CopyResource(m_sharedInD11.Get(), srcCapturedTex);
        }
    }
    else
    {
        // Hardware bilinear downscale to workWidth x workHeight using D3D11 pipeline
        if (srcCapturedSRV && m_sharedInRTV && m_downscaleVS && m_downscalePS)
        {
            D3D11_VIEWPORT vp = {};
            vp.Width    = static_cast<float>(m_workWidth);
            vp.Height   = static_cast<float>(m_workHeight);
            vp.MaxDepth = 1.0f;
            m_d3d11Ctx->RSSetViewports(1, &vp);
            m_d3d11Ctx->OMSetRenderTargets(1, m_sharedInRTV.GetAddressOf(), nullptr);

            m_d3d11Ctx->VSSetShader(m_downscaleVS.Get(), nullptr, 0);
            m_d3d11Ctx->PSSetShader(m_downscalePS.Get(), nullptr, 0);

            if (m_downscaleCB)
            {
                float cb[4] = { 1.0f / static_cast<float>(m_workWidth), 1.0f / static_cast<float>(m_workHeight), 0.0f, 0.0f };
                m_d3d11Ctx->UpdateSubresource(m_downscaleCB.Get(), 0, nullptr, cb, 0, 0);
                m_d3d11Ctx->PSSetConstantBuffers(0, 1, m_downscaleCB.GetAddressOf());
            }

            ID3D11ShaderResourceView* inSRVs[1] = { srcCapturedSRV };
            m_d3d11Ctx->PSSetShaderResources(0, 1, inSRVs);
            m_d3d11Ctx->PSSetSamplers(0, 1, m_downscaleSampler.GetAddressOf());

            m_d3d11Ctx->IASetInputLayout(nullptr);
            m_d3d11Ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            m_d3d11Ctx->Draw(3, 0);

            ID3D11ShaderResourceView* nullSRVs[1] = { nullptr };
            m_d3d11Ctx->PSSetShaderResources(0, 1, nullSRVs);
            ID3D11RenderTargetView* nullRTVs[1] = { nullptr };
            m_d3d11Ctx->OMSetRenderTargets(1, nullRTVs, nullptr);
        }
        else
        {
            m_d3d11Ctx->CopyResource(m_sharedInD11.Get(), srcCapturedTex);
        }
    }

    // 2. Optical Flow: Compute real-time Motion Vectors at model work resolution
    m_lastMvMs = 0.0;
    if (mvMgr && m_sharedInSRV && m_sharedMvUAV)
    {
        LARGE_INTEGER tMv0, tMv1, qpf;
        QueryPerformanceFrequency(&qpf);
        QueryPerformanceCounter(&tMv0);
        mvMgr->ProcessFrame(m_d3d11Ctx.Get(), m_sharedInSRV.Get(), m_sharedInD11.Get(), m_sharedMvUAV.Get(), m_sharedMvD11.Get());
        QueryPerformanceCounter(&tMv1);
        if (qpf.QuadPart > 0)
            m_lastMvMs = static_cast<double>(tMv1.QuadPart - tMv0.QuadPart) * 1000.0 / static_cast<double>(qpf.QuadPart);
    }

    if (m_frameIndex <= 3 || (m_frameIndex % 300 == 0))
    {
        DLSS_Log("[D3D12Interop] BeginFrame #%llu: mvMgr=%p, sharedInSRV=%p, sharedMvUAV=%p, sharedMvD12=%p",
            m_frameIndex, mvMgr, m_sharedInSRV.Get(), m_sharedMvUAV.Get(), m_sharedMvD12.Get());
    }

    // 3. Signal D3D11 Fence that copy/downscale AND optical flow are submitted to GPU
    m_d3d11Ctx4->Signal(m_fenceInD11.Get(), m_frameIndex);

    // Conditional Flush: Modern WDDM 2.0+ drivers automatically submit GPU fence signals without CPU stall.
    if (m_enableExplicitFlush)
    {
        m_d3d11Ctx->Flush();
    }

    // 4. DLSS kuyrugunun girisi beklemesi
    if (m_crossAdapter)
    {
        // A once D3D11'i bekler, sonra giris + hareket vektorlerini kopruye
        // yukler; B de o yuklemeyi bekler.
        if (!SubmitUploadToBridge())
        {
            DLSS_Log("[D3D12Interop] ERROR: Cross-adapter yukleme gonderilemedi (kare #%llu).", m_frameIndex);
            return false;
        }
        m_cmdQueue->Wait(m_xaFenceInB.Get(), m_frameIndex);
    }
    else
    {
        m_cmdQueue->Wait(m_fenceInD12.Get(), m_frameIndex);
    }

    // 5. Triple-buffered command allocator rotation with per-allocator fence tracking:
    // Before reusing this command allocator, check if its last submitted fence value has completed on GPU.
    // The CPU is ONLY stalled if the GPU has not finished yet (eliminating per-frame blocking).
    m_allocIndex = (m_allocIndex + 1) % kCmdAllocCount;
    // Not: cross-adapter modda m_fenceOutD12 A cihazinda yasar ve B'nin is
    // bitisini temsil etmez; her iki modda da B'nin yerel fence'i kullanilir.
    UINT64 neededFenceVal = m_allocFenceValue[m_allocIndex];
    if (m_gpuFence && neededFenceVal > 0 && m_gpuFence->GetCompletedValue() < neededFenceVal)
    {
        if (m_hFenceEvent)
        {
            m_gpuFence->SetEventOnCompletion(neededFenceVal, m_hFenceEvent);
            DWORD waitRes = WaitForSingleObject(m_hFenceEvent, 1000); // Only waits if GPU hasn't caught up
            if (waitRes == WAIT_TIMEOUT)
            {
                DLSS_Log("[D3D12Interop] WARNING: D3D12 GPU wait timeout (1000ms) waiting for fence %llu on alloc[%u] (frame #%llu)!",
                    neededFenceVal, m_allocIndex, m_frameIndex);
                if (m_d3d12Device)
                {
                    HRESULT rr = m_d3d12Device->GetDeviceRemovedReason();
                    if (FAILED(rr))
                    {
                        const char* rrDesc = "bilinmeyen hata";
                        switch (rr)
                        {
                        case DXGI_ERROR_DEVICE_HUNG:
                            rrDesc = "DEVICE_HUNG: GPU surucusu yanit vermedi (TDR tetiklendi)";
                            break;
                        case DXGI_ERROR_DEVICE_REMOVED:
                            rrDesc = "DEVICE_REMOVED: GPU fiziksel olarak kaldirildi veya surucusu yeniden yuklendi";
                            break;
                        case DXGI_ERROR_DEVICE_RESET:
                            rrDesc = "DEVICE_RESET: GPU surucusu sifirlandi (surucu çökmesi / kurtarma)";
                            break;
                        case DXGI_ERROR_DRIVER_INTERNAL_ERROR:
                            rrDesc = "DRIVER_INTERNAL_ERROR: NVIDIA surucu ic hatasi (BSOD riskteyiz!)";
                            break;
                        case DXGI_ERROR_INVALID_CALL:
                            rrDesc = "INVALID_CALL: gecersiz D3D12 cagri dizisi (uygulama hatasi)";
                            break;
                        default:
                            break;
                        }
                        DLSS_Log("[D3D12Interop] KRITIK: D3D12 Cihaz Kaldirildi! (0x%08X) %s", rr, rrDesc);
                    }
                }
                // Also check D3D11 device
                if (m_d3d11Dev)
                {
                    HRESULT rr11 = m_d3d11Dev->GetDeviceRemovedReason();
                    if (FAILED(rr11))
                        DLSS_Log("[D3D12Interop] KRITIK: D3D11 Cihaz da Kaldirildi! (0x%08X)", rr11);
                }
            }
        }
    }
    // Bu slotun onceki karesi GPU'da bitmis durumda (yukaridaki fence bunu garanti
    // eder), dolayisiyla zaman damgalari okunmaya hazir.
    if (m_tsHeap && m_tsReadback && neededFenceVal > 0)
    {
        const UINT slot = m_allocIndex * 2;
        D3D12_RANGE rr = { slot * sizeof(UINT64), (slot + 2) * sizeof(UINT64) };
        void* mapped = nullptr;
        if (SUCCEEDED(m_tsReadback->Map(0, &rr, &mapped)) && mapped)
        {
            const UINT64* ts = reinterpret_cast<const UINT64*>(mapped) + slot;
            if (ts[1] > ts[0] && m_tsFrequency)
                m_lastGpuMs = static_cast<double>(ts[1] - ts[0]) * 1000.0 / static_cast<double>(m_tsFrequency);
            D3D12_RANGE nowrite = { 0, 0 };
            m_tsReadback->Unmap(0, &nowrite);
        }
    }

    m_cmdAlloc[m_allocIndex]->Reset();
    m_cmdList->Reset(m_cmdAlloc[m_allocIndex].Get(), nullptr);

    if (m_tsHeap)
        m_cmdList->EndQuery(m_tsHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, m_allocIndex * 2);

    // Cross-adapter: kopru tamponlarindan B'deki yerel dokulara al.
    // DLSSNRManager giris/hareket kaynaklarini COMMON'da bekledigi icin
    // kopyadan sonra COMMON'a geri donuyoruz.
    if (m_crossAdapter)
    {
        D3D12_RESOURCE_BARRIER toCopy[2] = {};
        UINT n = 0;

        toCopy[n].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toCopy[n].Transition.pResource   = m_sharedInD12.Get();
        toCopy[n].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        toCopy[n].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
        toCopy[n].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        ++n;

        const bool haveMv = (m_sharedMvD12 && m_xaMv.bufB);
        if (haveMv)
        {
            toCopy[n].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            toCopy[n].Transition.pResource   = m_sharedMvD12.Get();
            toCopy[n].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
            toCopy[n].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
            toCopy[n].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            ++n;
        }
        m_cmdList->ResourceBarrier(n, toCopy);

        {
            D3D12_TEXTURE_COPY_LOCATION dst = {};
            dst.pResource        = m_sharedInD12.Get();
            dst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.SubresourceIndex = 0;

            D3D12_TEXTURE_COPY_LOCATION src = {};
            src.pResource       = m_xaIn.bufB.Get();
            src.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint = m_xaIn.fp;

            m_cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }

        if (haveMv)
        {
            D3D12_TEXTURE_COPY_LOCATION dst = {};
            dst.pResource        = m_sharedMvD12.Get();
            dst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.SubresourceIndex = 0;

            D3D12_TEXTURE_COPY_LOCATION src = {};
            src.pResource       = m_xaMv.bufB.Get();
            src.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint = m_xaMv.fp;

            m_cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }

        for (UINT i = 0; i < n; ++i)
        {
            std::swap(toCopy[i].Transition.StateBefore, toCopy[i].Transition.StateAfter);
        }
        m_cmdList->ResourceBarrier(n, toCopy);
    }

    return true;
}

bool D3D12Interop::EndFrame()
{
    if (!m_cmdList || !m_cmdQueue || !m_d3d11Ctx4) return false;
    if (!m_crossAdapter && !m_sharedOutD12) return false;

    // Cross-adapter: DLSS ciktisini (B'deki yerel doku) kopru tamponuna yaz.
    if (m_crossAdapter)
    {
        if (!m_nativeOutD12 || !m_xaOut.bufB) return false;

        D3D12_RESOURCE_BARRIER toSrc = {};
        toSrc.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toSrc.Transition.pResource   = m_nativeOutD12.Get();
        toSrc.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        toSrc.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
        toSrc.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        m_cmdList->ResourceBarrier(1, &toSrc);

        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource       = m_xaOut.bufB.Get();
        dst.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = m_xaOut.fp;

        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource        = m_nativeOutD12.Get();
        src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;

        m_cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

        std::swap(toSrc.Transition.StateBefore, toSrc.Transition.StateAfter);
        m_cmdList->ResourceBarrier(1, &toSrc);
    }
    // In fallback mode (when shared UAV texture is unsupported), perform hardware copy
    else if (!m_useDirectSharedOut)
    {
        if (!m_nativeOutD12) return false;

        // 1. Transition nativeOutD12 (COMMON -> COPY_SOURCE) and sharedOutD12 (COMMON -> COPY_DEST)
        D3D12_RESOURCE_BARRIER bCopy[2] = {};
        bCopy[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bCopy[0].Transition.pResource   = m_nativeOutD12.Get();
        bCopy[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        bCopy[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
        bCopy[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

        bCopy[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bCopy[1].Transition.pResource   = m_sharedOutD12.Get();
        bCopy[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        bCopy[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
        bCopy[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

        m_cmdList->ResourceBarrier(2, bCopy);

        // 2. Hardware copy DLSS-NR output into D3D11 shared texture
        m_cmdList->CopyResource(m_sharedOutD12.Get(), m_nativeOutD12.Get());

        // 3. Transition both back to COMMON state
        D3D12_RESOURCE_BARRIER bRestore[2] = {};
        bRestore[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bRestore[0].Transition.pResource   = m_nativeOutD12.Get();
        bRestore[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        bRestore[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_COMMON;
        bRestore[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

        bRestore[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bRestore[1].Transition.pResource   = m_sharedOutD12.Get();
        bRestore[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        bRestore[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_COMMON;
        bRestore[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

        m_cmdList->ResourceBarrier(2, bRestore);
    }
    // In Direct Zero-Copy mode (m_useDirectSharedOut == true):
    // DLSS-NR writes directly into m_sharedOutD12 (UAV). Transitions are already handled
    // within DLSSNRManager::Evaluate (to COMMON). No copy, no additional barriers!

    if (m_tsHeap && m_tsReadback)
    {
        m_cmdList->EndQuery(m_tsHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, m_allocIndex * 2 + 1);
        m_cmdList->ResolveQueryData(
            m_tsHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
            m_allocIndex * 2, 2,
            m_tsReadback.Get(), m_allocIndex * 2 * sizeof(UINT64));
    }

    // 4. Close command list and execute on D3D12 GPU queue
    m_cmdList->Close();
    ID3D12CommandList* lists[] = { m_cmdList.Get() };
    m_cmdQueue->ExecuteCommandLists(1, lists);

    // 5. B'nin yerel fence'i: ayirici rotasyonu ve WaitForGpu bunu izler.
    m_cmdQueue->Signal(m_gpuFence.Get(), m_frameIndex);

    // 6. Sonucu D3D11'e geri baglama
    if (m_crossAdapter)
    {
        // B bittigini cross-adapter fence ile duyurur; A ciktiyi kopruden
        // sunum dokusuna cekip m_fenceOutD12'yi kendi kuyrugunda sinyaller.
        m_cmdQueue->Signal(m_xaFenceOutB.Get(), m_frameIndex);
        if (!SubmitDownloadFromBridge())
        {
            DLSS_Log("[D3D12Interop] ERROR: Cross-adapter indirme gonderilemedi (kare #%llu).", m_frameIndex);
            return false;
        }
    }
    else
    {
        m_cmdQueue->Signal(m_fenceOutD12.Get(), m_frameIndex);
    }

    // 7. D3D11 baglami, sunum dokusunun hazir olmasini GPU tarafinda bekler.
    m_d3d11Ctx4->Wait(m_fenceOutD11.Get(), m_frameIndex);

    // 8. Track the submitted fence value for this allocator.
    // The CPU is NOT blocked here; GPU-side Wait (m_d3d11Ctx4->Wait) is sufficient,
    // allowing full CPU/GPU concurrency and asynchronous presentation.
    m_allocFenceValue[m_allocIndex] = m_frameIndex;

    return true;
}

void D3D12Interop::LogDiagnosticPixels(uint64_t frameCount)
{
    if (frameCount > 3) return; // Only log for the first 3 frames! Prevents periodic GPU stalls.
    if (!m_d3d11Ctx || !m_d3d11Dev || !m_sharedInD11 || !m_sharedOutD11 || m_width <= 0 || m_height <= 0) return;

    if (!m_diagStagingIn)
    {
        D3D11_TEXTURE2D_DESC sd = {};
        sd.Width          = 1;
        sd.Height         = 1;
        sd.MipLevels      = 1;
        sd.ArraySize      = 1;
        sd.Format         = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.Usage          = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        m_d3d11Dev->CreateTexture2D(&sd, nullptr, &m_diagStagingIn);
        m_d3d11Dev->CreateTexture2D(&sd, nullptr, &m_diagStagingOut);
    }
    if (!m_diagStagingIn || !m_diagStagingOut) return;

    D3D11_BOX box = { (UINT)m_width / 2, (UINT)m_height / 2, 0, (UINT)m_width / 2 + 1, (UINT)m_height / 2 + 1, 1 };
    m_d3d11Ctx->CopySubresourceRegion(m_diagStagingIn.Get(), 0, 0, 0, 0, m_sharedInD11.Get(), 0, &box);
    m_d3d11Ctx->CopySubresourceRegion(m_diagStagingOut.Get(), 0, 0, 0, 0, m_sharedOutD11.Get(), 0, &box);

    D3D11_MAPPED_SUBRESOURCE mapIn = {}, mapOut = {};
    if (SUCCEEDED(m_d3d11Ctx->Map(m_diagStagingIn.Get(), 0, D3D11_MAP_READ, 0, &mapIn)))
    {
        uint32_t pixIn = *(uint32_t*)mapIn.pData;
        m_d3d11Ctx->Unmap(m_diagStagingIn.Get(), 0);

        if (SUCCEEDED(m_d3d11Ctx->Map(m_diagStagingOut.Get(), 0, D3D11_MAP_READ, 0, &mapOut)))
        {
            uint32_t pixOut = *(uint32_t*)mapOut.pData;
            m_d3d11Ctx->Unmap(m_diagStagingOut.Get(), 0);

            DLSS_Log("[D3D12Interop] Frame #%llu Diagnostic Pixels @ (%d,%d): IN=0x%08X (R=%u,G=%u,B=%u) | OUT=0x%08X (R=%u,G=%u,B=%u)",
                frameCount, m_width / 2, m_height / 2,
                pixIn, (pixIn >> 16) & 0xFF, (pixIn >> 8) & 0xFF, pixIn & 0xFF,
                pixOut, (pixOut >> 16) & 0xFF, (pixOut >> 8) & 0xFF, pixOut & 0xFF);
        }
    }
}
