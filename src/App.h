#pragma once
#include "Common.h"
#include "CaptureManager.h"
#include "Renderer.h"
#include "InputForwarder.h"

// -----------------------------------------------------------------------
// App — state machine that owns the overlay window and the render loop.
// -----------------------------------------------------------------------
// -----------------------------------------------------------------------
enum class AppState { Menu, Capturing };
enum class CalibState { Idle, InitUncap, CoarseUp, FineDown, Done };

class App
{
public:
    explicit App(HINSTANCE hInstance);
    ~App();

    // Start WGC capture + overlay for the given target HWND.
    // 'menuHwnd' is the menu window (used to exclude from window list; hidden before this call).
    // Returns false on failure (shows menu again, caller logs the error).
    // fullscreenStretch: overlay hedef pencerenin degil, hedefin bulundugu MONITORUN
    // dikdortgenini kaplar ve yakalanan kare tam ekrana gerilir.
    bool StartOverlay(HWND menuHwnd, HWND targetHwnd, bool vsync = false, bool dlss = true, bool fps = true,
                      bool fullscreenStretch = false);

    // Run the overlay render loop until Alt+S is pressed or the target window closes.
    // Blocks on the calling thread (nested Win32 modal loop).
    void Run();

    AppState GetState() const { return m_state; }
    void RequestStop() { m_running = false; }

    // Update preferred GPU (resets D3D device if in menu so next capture uses new GPU)
    void SetPreferredGpu(const std::wstring& gpuName);

private:
    // D3D11 device shared for the lifetime of the app.
    bool InitD3D();

    // Create the borderless topmost overlay HWND.
    bool CreateOverlayWindow(HWND targetHwnd);

    // Keep the overlay rect in sync with the target window every frame.
    void UpdateOverlayPosition();
    // Overlay'in kaplamasi gereken ekran dikdortgeni.
    // Tam Ekran Yap acikken hedefin bulundugu monitorun tamami, kapaliyken hedefin
    // DWM cerceve siniri.
    RECT ComputeOverlayRect() const;

    // Recreate textures and swap chain after a WGC size change.
    void RecreateCaptureSizedResources();

    // Per-frame update + render.
    void Update();
    void Render(ID3D11ShaderResourceView* srv);

    // Poll the stop keybind each frame via GetAsyncKeyState (no hooks).
    void CheckStopKey();

    // Toggle between Overlay focus and Target App focus via F8 key.
    void CheckF8FocusToggle();

    // Auto-Hide overlay when target window loses focus or minimizes (Alt+Tab handling).
    void CheckFocusAndMinimize();

    // Tear down the overlay session and restore state.
    void StopOverlay();

    static LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    // ---- members ----
    HINSTANCE m_hInstance   = nullptr;
    HWND      m_overlayHwnd = nullptr;
    HWND      m_targetHwnd  = nullptr;
    HWND      m_menuHwnd    = nullptr;

    AppState  m_state            = AppState::Menu;
    bool      m_running          = false;
    bool      m_prevStopKeyDown  = false;
    bool      m_overlayFocused   = false;
    bool      m_prevFocusDown    = false;
    bool      m_overlayHidden    = false;
    ULONGLONG m_sessionStartTime = 0;

    // FPS tracking (measures overlay window's actual render FPS)
    LARGE_INTEGER m_fpsFreq       = {};
    LARGE_INTEGER m_fpsLastTime   = {};
    int           m_fpsFrames     = 0;
    float         m_fpsCurrent    = 0.0f;
    bool          m_fpsEnabled    = true;
    bool          m_prevFpsDown   = false;
    bool          m_dlssEnabled   = true;
    bool          m_prevVlssDown  = false;

    bool          m_fgMarkerActive      = false;
    // Oyun modunda OS imlecinin gizli tutulmasi icin thread imlec sayaci durumu.
    bool          m_cursorVisible       = true;
    bool          m_prevFgIndicatorDown = false;
    // Oturum basinda config'den okunur. true ise overlay WS_EX_LAYERED tasimaz
    // (Direct Flip acik, fare gecirgenligi yalnizca HTTRANSPARENT'a bagli).
    bool          m_directFlip          = false;
    bool          m_fullscreenStretch   = false;
    // Tam Ekran modunda hedefin hangi monitorde oldugunu takip eder; monitor
    // degismedikce overlay'i yeniden konumlandirmaya gerek yoktur.
    HMONITOR      m_lastMonitor         = nullptr;

    // Window border stripping
    LONG_PTR      m_originalTargetStyle   = 0;
    LONG_PTR      m_originalTargetExStyle = 0;
    RECT          m_originalTargetRect    = {};
    bool          m_bordersStripped       = false;

    // Warning OSD & Calibration
    bool          m_warningDismissed      = false;
    bool          m_prevCalibDown         = false;
    CalibState    m_calibState            = CalibState::Idle;
    int           m_calibTargetFps        = 0;
    ULONGLONG     m_calibTimer            = 0;
    std::wstring  m_calibMessage          = L"";
    ULONGLONG     m_calibMessageTimer     = 0;

    void StripTargetBorders();
    void RestoreTargetBorders();

    RECT m_lastTargetRect = {};

    ComPtr<ID3D11Device>        m_device;
    ComPtr<ID3D11DeviceContext> m_context;

    std::unique_ptr<CaptureManager> m_captureManager;
    std::unique_ptr<Renderer>       m_renderer;

    // ---- Per-frame pipeline timing ----
    struct FrameTimings
    {
        double captureWaitMs  = 0.0;
        double downscaleMs    = 0.0;
        double modelEvalMs    = 0.0;
        double presentMs      = 0.0;
        double totalMs        = 0.0;
    };

    // Running stats for the 5-second summary log
    double   m_sumTotalMs     = 0.0;
    double   m_maxTotalMs     = 0.0;
    uint64_t m_timingSamples  = 0;
    LARGE_INTEGER m_lastPerfLogTime = {};

    // Watchdog: tracks which pipeline stage is currently executing
    std::atomic<const char*> m_currentStage{ "idle" };
    std::atomic<bool>        m_watchdogRunning{ false };
    HANDLE                   m_watchdogThread = nullptr;
    LARGE_INTEGER            m_stageEnteredTime = {};

    static DWORD WINAPI WatchdogThreadProc(LPVOID param);
    void StartWatchdog();
    void StopWatchdog();
    void FlushPerfStats();
    void LogSystemInfo();
};
