#pragma once
#include "Common.h"
#include <dxgi1_6.h>

// ---------------------------------------------------------------------------
// GpuSelector
//   VLSS5 iki ayri GPU kullanabilir:
//     * Yakalama + sunum GPU'su  (ConfigManager: selectedGpu)
//       Monitoru suren kart olmak zorunda; WGC yakalamasi, swap chain ve OSD
//       bu cihazda calisir.
//     * DLSS / D3D12 GPU'su      (ConfigManager: dlssGpu)
//       Sinir agi degerlendirmesi burada kosar; RTX olmasi gerekir.
//
//   Iki secim ayni adapter'a cozulurse tek-adapter hizli yol devrededir.
//   Farkli cozulurse D3D12Interop cross-adapter kopru moduna gecer.
// ---------------------------------------------------------------------------
namespace GpuSelector
{
    // Verilen aciklamaya (substring) uyan adapter'i dondurur.
    // Bulunamazsa / L"Auto" ise once bir RTX karta, o da yoksa ilk donanim
    // adapter'ina duser. Hicbir donanim adapter'i yoksa nullptr doner.
    ComPtr<IDXGIAdapter1> FindAdapter(const std::wstring& targetGpuName);

    // Bir D3D11 cihazinin uzerinde kostugu adapter'in LUID'i.
    bool GetDeviceLuid(ID3D11Device* device, LUID& outLuid);

    // Iki LUID ayni fiziksel adapter'i mi gosteriyor?
    inline bool SameAdapter(const LUID& a, const LUID& b)
    {
        return a.LowPart == b.LowPart && a.HighPart == b.HighPart;
    }

    // Loglama / UI icin adapter aciklamasi. adapter nullptr ise bos string.
    std::wstring GetAdapterDescription(IDXGIAdapter* adapter);
}
