#include "GpuSelector.h"

namespace GpuSelector
{

ComPtr<IDXGIAdapter1> FindAdapter(const std::wstring& targetGpuName)
{
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        return nullptr;

    ComPtr<IDXGIAdapter1> matchedAdapter;
    ComPtr<IDXGIAdapter1> rtxAdapter;
    ComPtr<IDXGIAdapter1> firstHardwareAdapter;

    UINT i = 0;
    ComPtr<IDXGIAdapter1> adapter;
    while (factory->EnumAdapters1(i++, &adapter) != DXGI_ERROR_NOT_FOUND)
    {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);

        if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
        {
            if (!firstHardwareAdapter)
                firstHardwareAdapter = adapter;

            if (!rtxAdapter && (wcsstr(desc.Description, L"RTX") != nullptr || wcsstr(desc.Description, L"rtx") != nullptr))
                rtxAdapter = adapter;

            if (!targetGpuName.empty() && targetGpuName != L"Auto" && wcsstr(desc.Description, targetGpuName.c_str()) != nullptr)
            {
                matchedAdapter = adapter;
                break;
            }
        }
    }

    if (matchedAdapter)
        return matchedAdapter;

    if (rtxAdapter)
        return rtxAdapter;

    return firstHardwareAdapter;
}

bool GetDeviceLuid(ID3D11Device* device, LUID& outLuid)
{
    outLuid = {};
    if (!device) return false;

    ComPtr<IDXGIDevice> dxgiDev;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgiDev)))) return false;

    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgiDev->GetAdapter(&adapter))) return false;

    DXGI_ADAPTER_DESC desc{};
    if (FAILED(adapter->GetDesc(&desc))) return false;

    outLuid = desc.AdapterLuid;
    return true;
}

std::wstring GetAdapterDescription(IDXGIAdapter* adapter)
{
    if (!adapter) return std::wstring();

    DXGI_ADAPTER_DESC desc{};
    if (FAILED(adapter->GetDesc(&desc))) return std::wstring();

    return std::wstring(desc.Description);
}

} // namespace GpuSelector
