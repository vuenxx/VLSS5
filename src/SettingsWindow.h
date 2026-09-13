#pragma once
#include "Common.h"
#include "ConfigManager.h"
#include <functional>

class SettingsWindow
{
public:
    using ConfigChangedCallback = std::function<void(const Dlss5Config&)>;

    static void Initialize(HINSTANCE hInstance);
    static void Show(HWND parent = nullptr);
    static void Hide();
    static void Toggle(HWND parent = nullptr);
    static bool IsOpen();
    static HWND GetHwnd() { return s_hwnd; }

    static void SetOnConfigChanged(ConfigChangedCallback cb);

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK RebindKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK ModernSliderProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK ModernToggleProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    static void CreateControls(HWND hwnd);
    static void UpdateControlValues();
    static void UpdateLiveLabels();
    static void UpdateLayout();
    static void OnSettingChanged();
    static void StartKeybindCapture();
    static void EndKeybindCapture();

    static HWND                  s_hwnd;
    static HINSTANCE             s_hInstance;
    static bool                  s_rebindingKey;
    static HHOOK                 s_rebindHook;
    static ConfigChangedCallback s_callback;

    // Controls
    static HWND s_comboStyle;
    static HWND s_comboPreset;
    static HWND s_sliderIntensity;
    static HWND s_lblIntensityVal;
    static HWND s_sliderBoost;
    static HWND s_lblBoostVal;
    static HWND s_sliderStructure;
    static HWND s_lblStructureVal;
    static HWND s_sliderTone;
    static HWND s_lblToneVal;
    static HWND s_sliderSkin;
    static HWND s_lblSkinVal;
    static HWND s_sliderResScale;
    static HWND s_lblResScaleVal;
    static HWND s_chkAutoMask;
    static HWND s_chkStabilizer;
    static HWND s_chkSplitScreen;
    static HWND s_lblSplitTitle;
    static HWND s_lblSplitVal;
    static HWND s_sliderSplit;
    static HWND s_lblKeyTitle;
    static HWND s_lblHotkey;
    static HWND s_btnRebind;
    static HWND s_btnClose;

    // GDI resources
    static HFONT s_fontTitle;
    static HFONT s_fontNormal;
    static HFONT s_fontBold;
    static HFONT s_fontSmall;
    static HBRUSH s_brBg;
    static HBRUSH s_brCard;
    static HBRUSH s_brBorder;
};
