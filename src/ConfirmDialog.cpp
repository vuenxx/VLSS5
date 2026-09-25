#include "ConfirmDialog.h"
#include <commctrl.h>

#pragma comment(lib, "comctl32.lib")

namespace ConfirmDialog
{

bool AskYesNo(HWND parent, const wchar_t* windowTitle, const wchar_t* mainInstruction,
              const wchar_t* content, const wchar_t* yesLabel, const wchar_t* noLabel,
              bool defaultIsYes, bool warningIcon)
{
    typedef HRESULT (WINAPI *TaskDialogIndirect_t)(const TASKDIALOGCONFIG*, int*, int*, BOOL*);
    HMODULE hComCtl = LoadLibraryW(L"comctl32.dll");
    TaskDialogIndirect_t pTaskDialogIndirect =
        hComCtl ? (TaskDialogIndirect_t)GetProcAddress(hComCtl, "TaskDialogIndirect") : nullptr;

    static const int kYesId = 2001;
    static const int kNoId  = 2002;

    if (pTaskDialogIndirect)
    {
        TASKDIALOGCONFIG tdc = { sizeof(TASKDIALOGCONFIG) };
        tdc.hwndParent         = parent;
        tdc.hInstance          = GetModuleHandleW(nullptr);
        tdc.dwFlags            = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
        tdc.pszWindowTitle     = windowTitle;
        tdc.pszMainInstruction = mainInstruction;
        tdc.pszContent         = (content && content[0]) ? content : nullptr;
        tdc.pszMainIcon        = warningIcon ? TD_WARNING_ICON : TD_INFORMATION_ICON;

        TASKDIALOG_BUTTON buttons[] = {
            { kYesId, yesLabel },
            { kNoId,  noLabel  },
        };
        tdc.cButtons       = 2;
        tdc.pButtons       = buttons;
        tdc.nDefaultButton = defaultIsYes ? kYesId : kNoId;

        int selectedButton = 0;
        HRESULT hr = pTaskDialogIndirect(&tdc, &selectedButton, nullptr, nullptr);
        return SUCCEEDED(hr) && selectedButton == kYesId;
    }

    UINT flags = MB_YESNO | MB_TOPMOST | (warningIcon ? MB_ICONWARNING : MB_ICONQUESTION);
    if (!defaultIsYes) flags |= MB_DEFBUTTON2;

    std::wstring msg = mainInstruction ? mainInstruction : L"";
    if (content && content[0])
    {
        msg += L"\n\n";
        msg += content;
    }

    int res = MessageBoxW(parent, msg.c_str(), windowTitle, flags);
    return res == IDYES;
}

}
