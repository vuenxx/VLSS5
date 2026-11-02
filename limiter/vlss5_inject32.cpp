// ===========================================================================
// VLSS5 32-bit enjeksiyon yardimcisi (vlss5_inject32.exe)
//
// Kullanim:  vlss5_inject32.exe <pid> <dllYolu>
//
// 64-bit VLSS5.exe, hedef oyun 32-bit (WOW64) oldugunda bu Win32 exe'yi
// calistirir. Enjeksiyon KENDI mimarisinde (32-bit) yapildigi icin 64-bit->32-bit
// capraz LoadLibraryW adres problemi olmaz. Paylasimli bellegi ana uygulama
// zaten olusturdu; buradaki tek is 32-bit DLL'i hedefe yuklemek.
//
// Exit code: 0 = basari, !=0 = hata (LimiterManager::LaunchInjector32 okur).
//
// NE YAPAR: Yalnizca VLSS5'in kendi imzali modulunu (vlss5_limiter32.dll) hedefe
// yukler; oyun bellegini okumaz/yazmaz, baska hicbir sey yapmaz. 64-bit taraftaki
// LimiterManager::LoadRemoteModule ile AYNI standart dizidir, yalnizca 32-bit
// derlenir.
// ===========================================================================

#include <windows.h>
#include <string>
#include <cstdlib>

// ---------------------------------------------------------------------------
// Hedefe DLL yukleme.
//
// DOLDURULACAK: Govde, 64-bit taraftaki LimiterManager.cpp icindeki
// LoadRemoteModule ile BIREBIR AYNIDIR (hedef icinde bellek ayir -> DLL yolunu
// yaz -> uzaktan LoadLibraryW cagir -> thread'i bekle -> ayrilan bellegi birak).
// O fonksiyonun govdesini buraya oldugu gibi kopyala; mimari disinda fark yok.
// Doldurulana kadar yardimci hata (1) dondurur ve 32-bit enjeksiyon devre disidir.
// ---------------------------------------------------------------------------
static bool LoadRemoteModule(HANDLE process, const std::wstring& dllPath)
{
    const SIZE_T pathBytes = (dllPath.size() + 1) * sizeof(wchar_t);

    void* remote = VirtualAllocEx(process, nullptr, pathBytes,
                                  MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote)
    {
        return false;
    }

    bool ok = false;
    HANDLE thread = nullptr;

    if (!WriteProcessMemory(process, remote, dllPath.c_str(), pathBytes, nullptr))
    {
        goto cleanup;
    }

    {
        auto loadLib = reinterpret_cast<LPTHREAD_START_ROUTINE>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
        if (!loadLib)
        {
            goto cleanup;
        }

        thread = CreateRemoteThread(process, nullptr, 0, loadLib, remote, 0, nullptr);
        if (!thread)
        {
            goto cleanup;
        }

        if (WaitForSingleObject(thread, 10000) != WAIT_OBJECT_0)
        {
            goto cleanup;
        }

        ok = true;
    }

cleanup:
    if (thread) CloseHandle(thread);
    VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    return ok;
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 3)
        return 2; // kullanim hatasi

    const DWORD pid = (DWORD)_wtoi(argv[1]);
    const std::wstring dllPath = argv[2];
    if (pid == 0)
        return 2;

    if (GetFileAttributesW(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES)
        return 3; // DLL yok

    HANDLE process = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE |
        PROCESS_VM_READ | PROCESS_QUERY_INFORMATION,
        FALSE, pid);
    if (!process)
        return 4; // surece erisilemedi

    const bool ok = LoadRemoteModule(process, dllPath);
    CloseHandle(process);

    return ok ? 0 : 1;
}
