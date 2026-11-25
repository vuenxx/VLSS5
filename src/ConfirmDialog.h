#pragma once
#include "Common.h"

// ---------------------------------------------------------------------------
// ConfirmDialog
//   Paylasilan Evet/Hayir onay yardimcisi. TaskDialogIndirect dinamik yuklenir
//   (comctl32 v6 manifest gerekliligi yuzunden statik baglanti guvenilir degil).
//   Proc bulunamazsa MessageBoxW(MB_YESNO)'ya duser.
// ---------------------------------------------------------------------------
namespace ConfirmDialog
{
    // yesLabel butonuna basilirsa true doner.
    bool AskYesNo(HWND parent, const wchar_t* windowTitle, const wchar_t* mainInstruction,
                  const wchar_t* content, const wchar_t* yesLabel, const wchar_t* noLabel,
                  bool defaultIsYes, bool warningIcon);
}
