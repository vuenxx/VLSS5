#pragma once
#include "Common.h"

// ---------------------------------------------------------------------------
// ConfirmDialog
//   Main.cpp'deki CheckRivaTuner()/CheckRivaTunerRunning() icinde tekrar eden
//   dinamik TaskDialogIndirect yukleme desenini (comctl32 v6 manifest gerekliligi
//   yuzunden statik baglanti guvenilir degil) paylasilan tek bir Evet/Hayir
//   onay yardimcisina cikarir. Proc bulunamazsa MessageBoxW(MB_YESNO)'ya duser.
// ---------------------------------------------------------------------------
namespace ConfirmDialog
{
    // yesLabel butonuna basilirsa true doner.
    bool AskYesNo(HWND parent, const wchar_t* windowTitle, const wchar_t* mainInstruction,
                  const wchar_t* content, const wchar_t* yesLabel, const wchar_t* noLabel,
                  bool defaultIsYes, bool warningIcon);
}
