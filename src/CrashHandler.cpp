#include "CrashHandler.h"
#include <dbghelp.h>
#include <shlwapi.h>

#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "shlwapi.lib")

namespace
{
    void BuildDumpPath(wchar_t (&outPath)[MAX_PATH], const wchar_t* prefix)
    {
        wchar_t dir[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, dir, MAX_PATH);
        PathRemoveFileSpecW(dir);

        SYSTEMTIME st;
        GetLocalTime(&st);
        wchar_t fileName[128];
        swprintf_s(fileName, L"%s_%04u%02u%02u_%02u%02u%02u.dmp",
            prefix, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

        PathCombineW(outPath, dir, fileName);
    }

    void WriteMiniDump(const wchar_t* path, EXCEPTION_POINTERS* exceptionPointers, DWORD threadId)
    {
        HANDLE hFile = CreateFileW(path, GENERIC_WRITE, 0, nullptr,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE)
        {
            DLSS_Log("[CrashHandler] Dump dosyasi acilamadi: %ls (LastError=%lu)", path, GetLastError());
            return;
        }

        MINIDUMP_EXCEPTION_INFORMATION mdei = {};
        mdei.ThreadId          = threadId;
        mdei.ExceptionPointers = exceptionPointers;
        mdei.ClientPointers    = FALSE;

        const MINIDUMP_TYPE dumpType = static_cast<MINIDUMP_TYPE>(
            MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo);

        BOOL ok = MiniDumpWriteDump(
            GetCurrentProcess(), GetCurrentProcessId(), hFile, dumpType,
            exceptionPointers ? &mdei : nullptr, nullptr, nullptr);

        if (ok)
            DLSS_Log("[CrashHandler] Tanisal dump yazildi: %ls", path);
        else
            DLSS_Log("[CrashHandler] MiniDumpWriteDump basarisiz oldu (LastError=%lu)", GetLastError());

        CloseHandle(hFile);
    }

    LONG WINAPI UnhandledExceptionFilterImpl(EXCEPTION_POINTERS* exceptionPointers)
    {
        DLSS_Log("[CrashHandler] YAKALANMAMIS ISTISNA: Code=0x%08X Address=0x%p",
            exceptionPointers && exceptionPointers->ExceptionRecord
                ? exceptionPointers->ExceptionRecord->ExceptionCode : 0,
            exceptionPointers && exceptionPointers->ExceptionRecord
                ? exceptionPointers->ExceptionRecord->ExceptionAddress : nullptr);

        wchar_t path[MAX_PATH];
        BuildDumpPath(path, L"vlss5_crash");
        WriteMiniDump(path, exceptionPointers, GetCurrentThreadId());

        return EXCEPTION_EXECUTE_HANDLER;
    }
}

namespace CrashHandler
{
    void Install()
    {
        SetUnhandledExceptionFilter(UnhandledExceptionFilterImpl);
    }

    void WriteHangDump(HANDLE hStuckThread, const char* stageName)
    {
        if (!hStuckThread) return;

        DLSS_Log("[Watchdog] Render thread '%s' asamasinda takili -- tanisal dump aliniyor...", stageName);

        DWORD suspendCount = SuspendThread(hStuckThread);
        if (suspendCount == static_cast<DWORD>(-1))
        {
            DLSS_Log("[Watchdog] SuspendThread basarisiz oldu (LastError=%lu), dump atlandi.", GetLastError());
            return;
        }

        CONTEXT ctx = {};
        ctx.ContextFlags = CONTEXT_FULL;
        if (!GetThreadContext(hStuckThread, &ctx))
        {
            DLSS_Log("[Watchdog] GetThreadContext basarisiz oldu (LastError=%lu), dump atlandi.", GetLastError());
            ResumeThread(hStuckThread);
            return;
        }

        EXCEPTION_RECORD exRecord = {};
        exRecord.ExceptionCode = 0xA5A5A5A5; // Ozel "hang" isareti (gercek bir istisna degil)

        EXCEPTION_POINTERS exPointers = {};
        exPointers.ExceptionRecord = &exRecord;
        exPointers.ContextRecord   = &ctx;

        wchar_t path[MAX_PATH];
        BuildDumpPath(path, L"vlss5_hang");
        WriteMiniDump(path, &exPointers, GetThreadId(hStuckThread));

        ResumeThread(hStuckThread);
    }

    void HandleConfirmedHang(HANDLE hStuckThread, const char* stageName)
    {
        DLSS_Log("[Watchdog] KESIN DONMA: render thread '%s' asamasinda cok uzun suredir "
            "hicbir ilerleme kaydetmedi. Program otomatik olarak kapatilacak.", stageName);

        // Son bir teshis dump'i daha al (thread'i suspend/resume eder). Az sonra
        // sureci tamamen sonlandiracagimiz icin resume etmenin pratikte bir onemi
        // yok, ama WriteHangDump zaten kendi ResumeThread'ini cagirip temiz cikiyor.
        WriteHangDump(hStuckThread, stageName);

        wchar_t message[512];
        swprintf_s(message,
            L"VLSS5 yanıt vermiyor ('%hs' aşamasında takıldı) ve otomatik olarak kapatılacak.\n\n"
            L"Teşhis dosyası uygulama klasörüne kaydedildi (vlss5_hang_*.dmp).\n"
            L"VLSS5'i yeniden başlatabilirsiniz.",
            stageName);

        // MessageBoxTimeoutW belgelenmemis ama XP'den beri user32.dll'de kararli
        // sekilde var olan bir API: normal MessageBox gibi davranir, tek farki
        // belirtilen sure sonunda kullanici tiklamasa bile kendiliginden kapanmasi.
        // Boylece kullanici PC basinda degilse process bu ekranda sonsuza kadar
        // takili kalmaz -- bildirimi gösterip yine de kendini kapatir.
        using MessageBoxTimeoutFn = int (WINAPI*)(HWND, LPCWSTR, LPCWSTR, UINT, WORD, DWORD);
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        MessageBoxTimeoutFn pMsgBoxTimeout = user32
            ? reinterpret_cast<MessageBoxTimeoutFn>(GetProcAddress(user32, "MessageBoxTimeoutW"))
            : nullptr;

        if (pMsgBoxTimeout)
        {
            pMsgBoxTimeout(nullptr, message, L"VLSS5 - Donma Tespit Edildi",
                MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SYSTEMMODAL, 0, 8000);
        }
        else
        {
            MessageBoxW(nullptr, message, L"VLSS5 - Donma Tespit Edildi",
                MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SYSTEMMODAL);
        }

        DLSS_Log("[Watchdog] Surec TerminateProcess ile sonlandiriliyor.");
        TerminateProcess(GetCurrentProcess(), 1);
    }
}
