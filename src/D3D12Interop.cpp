#include "D3D12Interop.h"
#include "MotionVectorManager.h"
#include <d3dcompiler.h>
#include <cstdio>
#include <algorithm>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

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

    // Get the underlying DXGI Adapter from D3D11 device
    ComPtr<IDXGIDevice> dxgiDev;
    if (FAILED(m_d3d11Dev.As(&dxgiDev))) return false;

    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgiDev->GetAdapter(&adapter))) return false;

    DXGI_ADAPTER_DESC desc{};
    adapter->GetDesc(&desc);
    DLSS_Log("[D3D12Interop] Creating D3D12 device on adapter: %ls", desc.Description);

    // Create D3D12 Device on the same GPU adapter
    HRESULT hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_d3d12Device));
    if (FAILED(hr))
    {
        DLSS_Log("[D3D12Interop] ERROR: D3D12CreateDevice failed: 0x%08X", hr);
        return false;
    }

    // Create Direct Command Queue with elevated GPU scheduling priority (GLOBAL_REALTIME or HIGH)
    // so VLSS5's DLSS/Neural Rendering passes get prioritized by the WDDM GPU scheduler.
    D3D12_COMMAND_QUEUE_DESC qDesc = {};
    qDesc.Type     = D3D12_COMMAND_LIST_TYPE_DIRECT;
    qDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;

    // Check hardware/driver support for GLOBAL_REALTIME queue priority via CheckFeatureSupport
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
        // GLOBAL_REALTIME creation can fail if process lacks sufficient privileges; fallback to HIGH
        DLSS_Log("[D3D12Interop] GLOBAL_REALTIME queue creation failed (0x%08X), falling back to HIGH priority.", hr);
        qDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
        hr = m_d3d12Device->CreateCommandQueue(&qDesc, IID_PPV_ARGS(&m_cmdQueue));
    }

    if (FAILED(hr))
    {
        DLSS_Log("[D3D12Interop] ERROR: CreateCommandQueue failed: 0x%08X", hr);
        return false;
    }

    DLSS_Log("[D3D12Interop] Command queue created with %s priority.",
             (qDesc.Priority == D3D12_COMMAND_QUEUE_PRIORITY_GLOBAL_REALTIME) ? "GLOBAL_REALTIME" : "HIGH");

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
    // Input fence: D3D11 signals -> D3D12 waits
    HRESULT hr = m_d3d12Device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&m_fenceInD12));
    if (FAILED(hr)) return false;

    HANDLE hInFence = nullptr;
    hr = m_d3d12Device->CreateSharedHandle(m_fenceInD12.Get(), nullptr, GENERIC_ALL, nullptr, &hInFence);
    if (FAILED(hr)) return false;

    hr = m_d3d11Dev5->OpenSharedFence(hInFence, IID_PPV_ARGS(&m_fenceInD11));
    CloseHandle(hInFence);
    if (FAILED(hr)) return false;

    // Output fence: D3D12 signals -> D3D11 waits
    hr = m_d3d12Device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&m_fenceOutD12));
    if (FAILED(hr)) return false;

    HANDLE hOutFence = nullptr;
    hr = m_d3d12Device->CreateSharedHandle(m_fenceOutD12.Get(), nullptr, GENERIC_ALL, nullptr, &hOutFence);
    if (FAILED(hr)) return false;

    hr = m_d3d11Dev5->OpenSharedFence(hOutFence, IID_PPV_ARGS(&m_fenceOutD11));
    CloseHandle(hOutFence);
    if (FAILED(hr)) return false;

    return true;
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
    float4 PS(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
    {
        return gSrcTex.Sample(gLinear, uv);
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
    return SUCCEEDED(hr);
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

    // 2. Shared Output Texture (Read by D3D11 swap chain, receives copied frame from D3D12)
    D3D11_TEXTURE2D_DESC tdOut = {};
    tdOut.Width     = workWidth;
    tdOut.Height    = workHeight;
    tdOut.MipLevels = 1;
    tdOut.ArraySize = 1;
    tdOut.Format    = DXGI_FORMAT_B8G8R8A8_UNORM;
    tdOut.SampleDesc.Count = 1;
    tdOut.Usage     = D3D11_USAGE_DEFAULT;
    tdOut.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    tdOut.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;

    hr = m_d3d11Dev->CreateTexture2D(&tdOut, nullptr, &m_sharedOutD11);
    if (FAILED(hr))
    {
        DLSS_Log("[D3D12Interop] ERROR: Failed to create m_sharedOutD11: 0x%08X", hr);
        return false;
    }

    hr = m_d3d11Dev->CreateShaderResourceView(m_sharedOutD11.Get(), nullptr, &m_sharedOutSRV);
    if (FAILED(hr))
    {
        DLSS_Log("[D3D12Interop] ERROR: Failed to create m_sharedOutSRV: 0x%08X", hr);
        return false;
    }

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

    // 3. Native D3D12 Output Texture with FULL UAV support (for DLSS-NR neural evaluation)
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
        DLSS_Log("[D3D12Interop] ERROR: Failed to create m_nativeOutD12: 0x%08X", hr);
        return false;
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

    return true;
}

void D3D12Interop::WaitForGpu()
{
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
    m_cmdQueue.Reset();
    m_d3d12Device.Reset();

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
    if (mvMgr && m_sharedInSRV && m_sharedMvUAV)
    {
        mvMgr->ProcessFrame(m_d3d11Ctx.Get(), m_sharedInSRV.Get(), m_sharedInD11.Get(), m_sharedMvUAV.Get(), m_sharedMvD11.Get());
    }

    if (m_frameIndex <= 3 || (m_frameIndex % 300 == 0))
    {
        DLSS_Log("[D3D12Interop] BeginFrame #%llu: mvMgr=%p, sharedInSRV=%p, sharedMvUAV=%p, sharedMvD12=%p",
            m_frameIndex, mvMgr, m_sharedInSRV.Get(), m_sharedMvUAV.Get(), m_sharedMvD12.Get());
    }

    // 3. Signal D3D11 Fence that copy/downscale AND optical flow are submitted to GPU
    m_d3d11Ctx4->Signal(m_fenceInD11.Get(), m_frameIndex);

    // CRITICAL FIX: Flush D3D11 immediate context!
    m_d3d11Ctx->Flush();

    // 4. Queue D3D12 CommandQueue wait for D3D11 fence
    m_cmdQueue->Wait(m_fenceInD12.Get(), m_frameIndex);

    // 5. Triple-buffered command allocator rotation with per-allocator fence tracking:
    // Before reusing this command allocator, check if its last submitted fence value has completed on GPU.
    // The CPU is ONLY stalled if the GPU has not finished yet (eliminating per-frame blocking).
    m_allocIndex = (m_allocIndex + 1) % kCmdAllocCount;
    UINT64 neededFenceVal = m_allocFenceValue[m_allocIndex];
    if (m_fenceOutD12 && neededFenceVal > 0 && m_fenceOutD12->GetCompletedValue() < neededFenceVal)
    {
        if (m_hFenceEvent)
        {
            m_fenceOutD12->SetEventOnCompletion(neededFenceVal, m_hFenceEvent);
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
                        DLSS_Log("[D3D12Interop] CRITICAL: D3D12 Device Removed! Reason: 0x%08X", rr);
                    }
                }
            }
        }
    }
    m_cmdAlloc[m_allocIndex]->Reset();
    m_cmdList->Reset(m_cmdAlloc[m_allocIndex].Get(), nullptr);

    return true;
}

bool D3D12Interop::EndFrame()
{
    if (!m_cmdList || !m_cmdQueue || !m_d3d11Ctx4 || !m_nativeOutD12 || !m_sharedOutD12) return false;

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

    // 4. Close command list and execute on D3D12 GPU queue
    m_cmdList->Close();
    ID3D12CommandList* lists[] = { m_cmdList.Get() };
    m_cmdQueue->ExecuteCommandLists(1, lists);

    // 5. Signal D3D12 output fence
    m_cmdQueue->Signal(m_fenceOutD12.Get(), m_frameIndex);

    // 6. Queue D3D11 context wait for D3D12 to finish processing (GPU-side sync)
    m_d3d11Ctx4->Wait(m_fenceOutD11.Get(), m_frameIndex);

    // 7. Track the submitted fence value for this allocator.
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
